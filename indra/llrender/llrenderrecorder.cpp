/**
 * @file llrenderrecorder.cpp
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

#include "linden_common.h"
#include "llrenderrecorder.h"
#include "lltexturedata.h"

void LLRenderRecording::clear()
{
    mCmds.clear();
    mF.clear();
    mC.clear();
    mImages.clear();
    for (auto& p : mEndBound) p = nullptr;
    mEndImage0 = nullptr;
}

size_t LLRenderRecording::bytes() const
{
    return mCmds.size() * sizeof(Cmd) + mF.size() * sizeof(F32) + mC.size() + mImages.size() * sizeof(void*);
}

void LLRenderRecording::replay(LLRenderBackend* be, const std::function<void(U32)>& live) const
{
    if (!be) return;
    for (const Cmd& c : mCmds)
    {
        switch (c.op)
        {
        case OP_BEGIN:        be->begin(c.u[0]); break;
        case OP_END:          be->end(); break;
        case OP_VERTEX:       be->vertex3f(c.f[0], c.f[1], c.f[2]); break;
        case OP_TEXCOORD:     be->texCoord2f(c.f[0], c.f[1]); break;
        case OP_COLOR4UB:     be->color4ub(c.b[0], c.b[1], c.b[2], (U8)c.u[0]); break;
        case OP_COLOR4F:      be->color4f(c.f[0], c.f[1], c.f[2], c.f[3]); break;
        case OP_BATCH:
        {
            const S32 n = (S32)c.u[1];
            const F32* pos = mF.data() + c.u[0];
            be->vertexBatchPreTransformed(pos, pos + 3 * n, mC.data() + (size_t)c.f[0], n);
            break;
        }
        case OP_MATRIX_MODE:  be->matrixMode(c.u[0]); break;
        case OP_PUSH:         be->pushMatrix(); break;
        case OP_POP:          be->popMatrix(); break;
        case OP_IDENTITY:     be->loadIdentity(); break;
        case OP_LOAD:         be->loadMatrix(mF.data() + c.u[0]); break;
        case OP_MULT:         be->multMatrix(mF.data() + c.u[0]); break;
        case OP_TRANSLATE:    be->translatef(c.f[0], c.f[1], c.f[2]); break;
        case OP_SCALE:        be->scalef(c.f[0], c.f[1], c.f[2]); break;
        case OP_ROTATE:       be->rotatef(c.f[0], c.f[1], c.f[2], c.f[3]); break;
        case OP_ORTHO:        be->ortho(c.f[0], c.f[1], c.f[2], c.f[3], c.f[4], c.f[5]); break;
        case OP_BLEND_FUNC:   be->blendFunc(c.u[0], c.u[1]); break;
        case OP_SCENE_BLEND:  be->setSceneBlendType(c.u[0]); break;
        case OP_COLOR_MASK2:  be->setColorMask(c.b[0] != 0, c.b[1] != 0); break;
        case OP_COLOR_MASK4:  be->setColorMask(c.b[0] != 0, c.b[1] != 0, c.b[2] != 0, c.u[0] != 0); break;
        case OP_LINE_WIDTH:   be->setLineWidth(c.f[0]); break;
        case OP_FLUSH:        be->flush(); break;
        case OP_DEPTH:        be->setDepthTest(c.b[0] != 0, c.b[1] != 0, c.u[0]); break;
        case OP_SCISSOR:      be->setScissor(c.b[0] != 0, (S32)c.u[0], (S32)c.u[1], (S32)c.f[0], (S32)c.f[1]); break;
        case OP_ACTIVE_UNIT:  be->setActiveTextureUnit(c.u[0]); break;
        case OP_BIND:         be->bindImageGL(c.u[0] ? mImages[c.u[0] - 1].get() : nullptr); break;
        case OP_UNBIND:       be->unbindTexture(); break;
        case OP_FILTER:       be->setTextureFilter(c.u[0]); break;
        case OP_ADDRESS:      be->setTextureAddressMode(c.u[0]); break;
        case OP_LIVE:         if (live) live(c.u[0]); break;
        default: break;
        }
    }
}

LLRenderRecording::Cmd& LLRenderRecorder::add(U8 op)
{
    if (mPaused)
    {
        static thread_local LLRenderRecording::Cmd s_scratch;
        s_scratch.op = op;
        return s_scratch;
    }
    mOut->mCmds.emplace_back();
    LLRenderRecording::Cmd& c = mOut->mCmds.back();
    c.op = op;
    return c;
}

void LLRenderRecorder::begin(U32 mode) { add(LLRenderRecording::OP_BEGIN).u[0] = mode; mReal->begin(mode); }
void LLRenderRecorder::end() { add(LLRenderRecording::OP_END); mReal->end(); }
void LLRenderRecorder::vertex3f(F32 x, F32 y, F32 z)
{
    auto& c = add(LLRenderRecording::OP_VERTEX); c.f[0] = x; c.f[1] = y; c.f[2] = z;
    mReal->vertex3f(x, y, z);
}
void LLRenderRecorder::texCoord2f(F32 u, F32 v)
{
    auto& c = add(LLRenderRecording::OP_TEXCOORD); c.f[0] = u; c.f[1] = v;
    mReal->texCoord2f(u, v);
}
void LLRenderRecorder::color4ub(U8 r, U8 g, U8 b, U8 a)
{
    auto& c = add(LLRenderRecording::OP_COLOR4UB); c.b[0] = r; c.b[1] = g; c.b[2] = b; c.u[0] = a;
    mReal->color4ub(r, g, b, a);
}
void LLRenderRecorder::color4f(F32 r, F32 g, F32 b, F32 a)
{
    auto& c = add(LLRenderRecording::OP_COLOR4F); c.f[0] = r; c.f[1] = g; c.f[2] = b; c.f[3] = a;
    mReal->color4f(r, g, b, a);
}
void LLRenderRecorder::vertexBatchPreTransformed(const F32* pos, const F32* uv, const U8* col, S32 count)
{
    if (count > 0 && !mPaused)
    {
        auto& c = add(LLRenderRecording::OP_BATCH);
        c.u[0] = (U32)mOut->mF.size();
        c.u[1] = (U32)count;
        c.f[0] = (F32)mOut->mC.size();   // exact up to 2^24 bytes of colour per recording
        mOut->mF.insert(mOut->mF.end(), pos, pos + 3 * count);
        mOut->mF.insert(mOut->mF.end(), uv, uv + 2 * count);
        mOut->mC.insert(mOut->mC.end(), col, col + 4 * count);
        if (mOut->mC.size() >= (1u << 24)) mBroken = true;
    }
    mReal->vertexBatchPreTransformed(pos, uv, col, count);
}
void LLRenderRecorder::matrixMode(U32 mode) { add(LLRenderRecording::OP_MATRIX_MODE).u[0] = mode; mReal->matrixMode(mode); }
void LLRenderRecorder::pushMatrix() { add(LLRenderRecording::OP_PUSH); mReal->pushMatrix(); }
void LLRenderRecorder::popMatrix() { add(LLRenderRecording::OP_POP); mReal->popMatrix(); }
void LLRenderRecorder::loadIdentity() { add(LLRenderRecording::OP_IDENTITY); mReal->loadIdentity(); }
void LLRenderRecorder::loadMatrix(const F32* m16)
{
    add(LLRenderRecording::OP_LOAD).u[0] = (U32)mOut->mF.size();
    if (!mPaused) mOut->mF.insert(mOut->mF.end(), m16, m16 + 16);
    mReal->loadMatrix(m16);
}
void LLRenderRecorder::multMatrix(const F32* m16)
{
    add(LLRenderRecording::OP_MULT).u[0] = (U32)mOut->mF.size();
    if (!mPaused) mOut->mF.insert(mOut->mF.end(), m16, m16 + 16);
    mReal->multMatrix(m16);
}
void LLRenderRecorder::translatef(F32 x, F32 y, F32 z)
{
    auto& c = add(LLRenderRecording::OP_TRANSLATE); c.f[0] = x; c.f[1] = y; c.f[2] = z;
    mReal->translatef(x, y, z);
}
void LLRenderRecorder::scalef(F32 x, F32 y, F32 z)
{
    auto& c = add(LLRenderRecording::OP_SCALE); c.f[0] = x; c.f[1] = y; c.f[2] = z;
    mReal->scalef(x, y, z);
}
void LLRenderRecorder::rotatef(F32 deg, F32 x, F32 y, F32 z)
{
    auto& c = add(LLRenderRecording::OP_ROTATE); c.f[0] = deg; c.f[1] = x; c.f[2] = y; c.f[3] = z;
    mReal->rotatef(deg, x, y, z);
}
void LLRenderRecorder::ortho(F32 l, F32 r, F32 b, F32 t, F32 zn, F32 zf)
{
    auto& c = add(LLRenderRecording::OP_ORTHO);
    c.f[0] = l; c.f[1] = r; c.f[2] = b; c.f[3] = t; c.f[4] = zn; c.f[5] = zf;
    mReal->ortho(l, r, b, t, zn, zf);
}
void LLRenderRecorder::blendFunc(U32 s, U32 d)
{
    auto& c = add(LLRenderRecording::OP_BLEND_FUNC); c.u[0] = s; c.u[1] = d;
    mReal->blendFunc(s, d);
}
void LLRenderRecorder::setSceneBlendType(U32 type) { add(LLRenderRecording::OP_SCENE_BLEND).u[0] = type; mReal->setSceneBlendType(type); }
void LLRenderRecorder::setColorMask(bool writeColor, bool writeAlpha)
{
    auto& c = add(LLRenderRecording::OP_COLOR_MASK2); c.b[0] = writeColor; c.b[1] = writeAlpha;
    mReal->setColorMask(writeColor, writeAlpha);
}
void LLRenderRecorder::setColorMask(bool r, bool g, bool b, bool a)
{
    auto& c = add(LLRenderRecording::OP_COLOR_MASK4); c.b[0] = r; c.b[1] = g; c.b[2] = b; c.u[0] = a;
    mReal->setColorMask(r, g, b, a);
}
void LLRenderRecorder::setLineWidth(F32 w) { add(LLRenderRecording::OP_LINE_WIDTH).f[0] = w; mReal->setLineWidth(w); }
void LLRenderRecorder::flush() { add(LLRenderRecording::OP_FLUSH); mReal->flush(); }
void LLRenderRecorder::setDepthTest(bool enabled, bool writeEnabled, U32 func)
{
    auto& c = add(LLRenderRecording::OP_DEPTH); c.b[0] = enabled; c.b[1] = writeEnabled; c.u[0] = func;
    mReal->setDepthTest(enabled, writeEnabled, func);
}
void LLRenderRecorder::setScissor(bool enable, S32 x, S32 y, S32 w, S32 h)
{
    auto& c = add(LLRenderRecording::OP_SCISSOR);
    c.b[0] = enable; c.u[0] = (U32)x; c.u[1] = (U32)y; c.f[0] = (F32)w; c.f[1] = (F32)h;
    mReal->setScissor(enable, x, y, w, h);
}
void LLRenderRecorder::setActiveTextureUnit(U32 unit) { add(LLRenderRecording::OP_ACTIVE_UNIT).u[0] = unit; mReal->setActiveTextureUnit(unit); }
void LLRenderRecorder::bindImageGL(LLTextureData* image)
{
    U32 idx = 0;
    if (image && !mPaused)
    {
        // Reuse the slot when the same image is bound again (glyph atlas, icons).
        auto& imgs = mOut->mImages;
        for (size_t i = imgs.size(); i-- > 0 && imgs.size() - i <= 8;)
            if (imgs[i].get() == image) { idx = (U32)i + 1; break; }
        if (!idx) { imgs.emplace_back(image); idx = (U32)imgs.size(); }
    }
    add(LLRenderRecording::OP_BIND).u[0] = idx;
    mReal->bindImageGL(image);
}
void LLRenderRecorder::unbindTexture() { add(LLRenderRecording::OP_UNBIND); mReal->unbindTexture(); }
void LLRenderRecorder::setTextureFilter(U32 filter) { add(LLRenderRecording::OP_FILTER).u[0] = filter; mReal->setTextureFilter(filter); }
void LLRenderRecorder::setTextureAddressMode(U32 mode) { add(LLRenderRecording::OP_ADDRESS).u[0] = mode; mReal->setTextureAddressMode(mode); }

void LLRenderRecorder::addLive(U32 index)
{
    if (mPaused) return;
    add(LLRenderRecording::OP_LIVE).u[0] = index;
}
