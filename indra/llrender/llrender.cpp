 /**
 * @file llrender.cpp
 * @brief LLRender implementation
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

#include "linden_common.h"

#include "llrender.h"
#include "llrenderrecorder.h"
#include "llrenderbackend.h"

#include "lltexturedata.h"
#include "lltexture.h"
#include "glm/gtc/type_ptr.hpp"

thread_local LLRender gGL;

// Handy copies of last good GL matrices
F32 gGLModelView[16];
F32 gGLLastModelView[16];
F32 gGLLastProjection[16];
F32 gGLProjection[16];

// transform from last frame's camera space to this frame's camera space (and inverse)
glm::mat4 gGLDeltaModelView;
glm::mat4 gGLInverseDeltaModelView;

S32 gGLViewport[4];


LLVector2 LLRender::sUIGLScaleFactor = LLVector2(1.f, 1.f);

namespace
{
// The backend LLRender talks to while no real one is installed (before the Vulkan
// UI facade is up, after it is torn down): every call is a no-op. LLRender::sBackend
// is never null, so no forwarder has to check.
class LLNullRenderBackend final : public LLRenderBackend
{
public:
    void beginFrame(U32, U32) override {}
    void endFrame() override {}
    void begin(U32) override {}
    void end() override {}
    void vertex3f(F32, F32, F32) override {}
    void texCoord2f(F32, F32) override {}
    void color4ub(U8, U8, U8, U8) override {}
    void color4f(F32, F32, F32, F32) override {}
    void vertexBatchPreTransformed(const F32*, const F32*, const U8*, S32) override {}
    void matrixMode(U32) override {}
    void pushMatrix() override {}
    void popMatrix() override {}
    void loadIdentity() override {}
    void loadMatrix(const F32*) override {}
    void multMatrix(const F32*) override {}
    void translatef(F32, F32, F32) override {}
    void scalef(F32, F32, F32) override {}
    void rotatef(F32, F32, F32, F32) override {}
    void ortho(F32, F32, F32, F32, F32, F32) override {}
    void blendFunc(U32, U32) override {}
    void setSceneBlendType(U32) override {}
    void setColorMask(bool, bool) override {}
    void setColorMask(bool, bool, bool, bool) override {}
    void setLineWidth(F32) override {}
    void flush() override {}
    void setDepthTest(bool, bool, U32) override {}
    void setScissor(bool, S32, S32, S32, S32) override {}
    void setActiveTextureUnit(U32) override {}
    void bindImageGL(LLTextureData*) override {}
    void unbindTexture() override {}
    void setTextureFilter(U32) override {}
    void setTextureAddressMode(U32) override {}
};

constinit LLNullRenderBackend sNullBackend;   // no static-init-order window
}

LLRenderBackend* LLRender::sBackend = &sNullBackend;
bool LLRender::sBackendInstalled = false;
std::vector<LLRenderCapturedBatch>* LLRender::sBackendCapture = nullptr;
bool LLRender::sBackendCaptureIncomplete = false;
LLTextureData* LLRender::sBackendBoundImage0 = nullptr;

void LLRender::beginBackendCapture(std::vector<LLRenderCapturedBatch>* out)
{
    sBackendCapture = out;
    sBackendCaptureIncomplete = false;
}

bool LLRender::endBackendCapture()
{
    const bool ok = sBackendCapture && !sBackendCaptureIncomplete;
    sBackendCapture = nullptr;
    return ok;
}

void LLRender::replayBackendCapture(const std::vector<LLRenderCapturedBatch>& batches)
{
    for (const LLRenderCapturedBatch& b : batches)
    {
        if (b.mImage.notNull()) gGL.getTexUnit(0)->bind(b.mImage.get());
        else                    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        gGL.begin((U8)b.mMode);
        sBackend->vertexBatchPreTransformed(b.mPos.data(), b.mUV.data(), b.mCol.data(),
                                            (S32)(b.mPos.size() / 3));
        gGL.end();
    }
}
static LLRenderBackend* sRetainedReal = nullptr;
static LLRenderRecorder* sRetainedRecorder = nullptr;
static LLRenderRecording* sRetainedOut = nullptr;

bool LLRender::beginRetainedCapture(LLRenderRecording* out)
{
    if (!sBackendInstalled || !out || sRetainedRecorder) return false;
    out->clear();
    sRetainedReal = sBackend;
    sRetainedOut = out;
    sRetainedRecorder = new LLRenderRecorder(sBackend, out);
    sBackend = sRetainedRecorder;
    return true;
}

bool LLRender::endRetainedCapture()
{
    if (!sRetainedRecorder) return false;
    const bool ok = !sRetainedRecorder->broken();
    sBackend = sRetainedReal;
    delete sRetainedRecorder;
    sRetainedRecorder = nullptr;
    LLRenderRecording* out = sRetainedOut;
    sRetainedOut = nullptr;
    sRetainedReal = nullptr;
    if (!ok) { out->clear(); return false; }
    for (U32 i = 0; i < LLRenderRecording::kTrackedUnits; ++i)
        out->mEndBound[i] = gGL.getTexUnit(i) ? gGL.getTexUnit(i)->mVKBound : nullptr;
    out->mEndImage0 = sBackendBoundImage0;
    return true;
}

bool LLRender::retainedCaptureActive()
{
    return sRetainedRecorder && !sRetainedRecorder->paused();
}

void LLRender::recordRetainedLive(U32 index)
{
    if (sRetainedRecorder) sRetainedRecorder->addLive(index);
}

void LLRender::pauseRetainedCapture(bool pause)
{
    if (sRetainedRecorder) sRetainedRecorder->setPaused(pause);
}

void LLRender::replayRetained(const LLRenderRecording& rec, const std::function<void(U32)>& live)
{
    rec.replay(sBackend, live);
    for (U32 i = 0; i < LLRenderRecording::kTrackedUnits; ++i)
        if (LLTexUnit* u = gGL.getTexUnit(i)) u->mVKBound = rec.mEndBound[i].get();
    sBackendBoundImage0 = rec.mEndImage0.get();
}

void LLRender::setBackend(LLRenderBackend* backend)
{
    llassert(!sRetainedRecorder);   // never swap the backend under a retained capture
    sBackend = backend ? backend : &sNullBackend;
    sBackendInstalled = (backend != nullptr);
}

LLTexUnit::LLTexUnit(S32 index)
    : mCurrTexType(TT_NONE),
    mIndex(index)
{
    llassert_always(index < (S32)LL_NUM_TEXTURE_LAYERS);
}

void LLTexUnit::enable(eTextureType type)
{
    if (mIndex < 0) return;

    if (mCurrTexType != type && type != TT_NONE)
    {
        if (mCurrTexType != TT_NONE)
        {
            disable(); // Force a disable of a previous texture type if it's enabled.
        }
        mCurrTexType = type;

        gGL.flush();
    }
}

void LLTexUnit::disable(void)
{
    if (mIndex < 0) return;

    if (mCurrTexType != TT_NONE)
    {
        unbind(mCurrTexType);
        mCurrTexType = TT_NONE;
    }
}

bool LLTexUnit::bind(LLTexture* texture, bool for_rendering, bool forceBind)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    // Resolve the LLTextureData to a backend image/descriptor and bind it to this unit.
    // The backend uploads pixels on first bind and caches by image.
    if (mIndex < 0) return false;
    LLTextureData* gl_tex = texture ? texture->getTextureData() : nullptr;
    if (gl_tex) gl_tex->setVKUiKind(texture->getVKUiKind());   // #405: UI VRAM breakdown
    LLRender::sBackend->setActiveTextureUnit((U32)mIndex);
    LLRender::sBackend->bindImageGL(gl_tex);
    // #707: the UI had nothing to upload (copy dropped, its UI image evicted): re-source
    if (gl_tex && gl_tex->takeVKUiNeedsCpu()) texture->vkRequestCpuCopy();
    LLRender::sBackend->setTextureAddressMode(gl_tex ? (U32)gl_tex->mVKAddressMode : (U32)TAM_CLAMP);
    mVKBound = gl_tex;
    if (mIndex == 0) LLRender::sBackendBoundImage0 = gl_tex;
    return gl_tex != nullptr;
}

bool LLTexUnit::bind(LLTextureData* texture, bool for_rendering, bool forceBind, S32 usename)
{
    // Bind the LLTextureData directly (font glyph atlas + UI icons take this path). The
    // backend uploads on first bind and refreshes when the atlas grows.
    if (mIndex < 0 || !texture) return false;
    LLRender::sBackend->setActiveTextureUnit((U32)mIndex);
    LLRender::sBackend->bindImageGL(texture);
    LLRender::sBackend->setTextureAddressMode((U32)texture->mVKAddressMode);
    mVKBound = texture;
    if (mIndex == 0) LLRender::sBackendBoundImage0 = texture;
    return true;
}

void LLTexUnit::unbind(eTextureType type)
{
    if (mIndex < 0) return;
    LLRender::sBackend->setActiveTextureUnit((U32)mIndex);
    LLRender::sBackend->unbindTexture();
    LLRender::sBackend->setTextureAddressMode((U32)TAM_CLAMP);
    mVKBound = nullptr;
    if (mIndex == 0) LLRender::sBackendBoundImage0 = nullptr;
}

void LLTexUnit::setTextureAddressMode(eTextureAddressMode mode)
{
    // The mode belongs to the bound texture: record it on the image so a later re-bind
    // keeps it, and apply it to this unit now. Tiled UI art (the transparency
    // checkerboard) relies on TAM_WRAP.
    if (mIndex < 0) return;
    if (mVKBound) mVKBound->mVKAddressMode = mode;
    LLRender::sBackend->setActiveTextureUnit((U32)mIndex);
    LLRender::sBackend->setTextureAddressMode((U32)mode);
}

void LLTexUnit::setTextureFilteringOption(LLTexUnit::eTextureFilterOptions option)
{
    if (mIndex < 0) return;
    LLRender::sBackend->setActiveTextureUnit((U32)mIndex);
    // TFO_POINT -> point/nearest (0); anything else -> bilinear/linear (1).
    LLRender::sBackend->setTextureFilter(option == TFO_POINT ? 0u : 1u);
}

LLRender::LLRender()
  : mMatrixMode(LLRender::MM_MODELVIEW),
    mMode(LLRender::TRIANGLES)
{
    for (U32 i = 0; i < LL_NUM_TEXTURE_LAYERS; i++)
    {
        mTexUnits[i].mIndex = i;
    }
}

LLRender::~LLRender()
{
}

void LLRender::translatef(const F32& x, const F32& y, const F32& z)
{
    sBackend->translatef(x, y, z);
}

void LLRender::scalef(const F32& x, const F32& y, const F32& z)
{
    sBackend->scalef(x, y, z);
}

void LLRender::ortho(F32 left, F32 right, F32 bottom, F32 top, F32 zNear, F32 zFar)
{
    sBackend->ortho(left, right, bottom, top, zNear, zFar);
}

void LLRender::rotatef(const F32& a, const F32& x, const F32& y, const F32& z)
{
    sBackend->rotatef(a, x, y, z);
}

void LLRender::pushMatrix()
{
    sBackend->pushMatrix();
}

void LLRender::popMatrix()
{
    sBackend->popMatrix();
}

void LLRender::loadMatrix(const F32* m)
{
    sBackend->loadMatrix(m);
}

void LLRender::multMatrix(const F32* m)
{
    sBackend->multMatrix(m);
}

void LLRender::matrixMode(eMatrixMode mode)
{
    sBackend->matrixMode((U32)mode);
    mMatrixMode = (mode == MM_TEXTURE) ? MM_TEXTURE0 : mode;
}

LLRender::eMatrixMode LLRender::getMatrixMode()
{
    if (mMatrixMode >= MM_TEXTURE0 && mMatrixMode <= MM_TEXTURE3)
    { //always return MM_TEXTURE if current matrix mode points at any texture matrix
        return MM_TEXTURE;
    }

    return mMatrixMode;
}

void LLRender::loadIdentity()
{
    sBackend->loadIdentity();
}

void LLRender::translateUI(F32 x, F32 y, F32 z)
{
    if (mUIOffset.empty())
    {
        LL_ERRS() << "Need to push a UI translation frame before offsetting" << LL_ENDL;
    }

    mUIOffset.back().add(LLVector4a(x, y, z));
}

void LLRender::scaleUI(F32 x, F32 y, F32 z)
{
    if (mUIScale.empty())
    {
        LL_ERRS() << "Need to push a UI transformation frame before scaling." << LL_ENDL;
    }

    mUIScale.back().mul(LLVector4a(x, y, z));
}

void LLRender::pushUIMatrix()
{
    if (mUIOffset.empty())
    {
        mUIOffset.emplace_back(0.f);
    }
    else
    {
        mUIOffset.push_back(mUIOffset.back());
    }

    if (mUIScale.empty())
    {
        mUIScale.emplace_back(1.f);
    }
    else
    {
        mUIScale.push_back(mUIScale.back());
    }
}

void LLRender::popUIMatrix()
{
    if (mUIOffset.empty())
    {
        LL_ERRS() << "UI offset stack blown." << LL_ENDL;
    }
    mUIOffset.pop_back();
    mUIScale.pop_back();
}

LLVector3 LLRender::getUITranslation()
{
    if (mUIOffset.empty())
    {
        return LLVector3::zero;
    }

    return LLVector3(mUIOffset.back().getF32ptr());
}

LLVector3 LLRender::getUIScale()
{
    if (mUIScale.empty())
    {
        return LLVector3::all_one;
    }

    return LLVector3(mUIScale.back().getF32ptr());
}


void LLRender::loadUIIdentity()
{
    if (mUIOffset.empty())
    {
        LL_ERRS() << "Need to push UI translation frame before clearing offset." << LL_ENDL;
    }

    mUIOffset.back().clear();
    mUIScale.back().splat(1);
}

void LLRender::setColorMask(bool writeColor, bool writeAlpha)
{
    setColorMask(writeColor, writeColor, writeColor, writeAlpha);
}

void LLRender::setColorMask(bool writeColorR, bool writeColorG, bool writeColorB, bool writeAlpha)
{
    sBackend->setColorMask(writeColorR, writeColorG, writeColorB, writeAlpha);
}

void LLRender::setSceneBlendType(eBlendType type)
{
    sBackend->setSceneBlendType((U32)type);
}

void LLRender::blendFunc(eBlendFactor sfactor, eBlendFactor dfactor)
{
    llassert(sfactor < BF_UNDEF);
    llassert(dfactor < BF_UNDEF);
    sBackend->blendFunc((U32)sfactor, (U32)dfactor);
}

void LLRender::blendFunc(eBlendFactor color_sfactor, eBlendFactor color_dfactor,
             eBlendFactor alpha_sfactor, eBlendFactor alpha_dfactor)
{
    llassert(color_sfactor < BF_UNDEF);
    llassert(color_dfactor < BF_UNDEF);
    llassert(alpha_sfactor < BF_UNDEF);
    llassert(alpha_dfactor < BF_UNDEF);
    sBackend->blendFunc((U32)color_sfactor, (U32)color_dfactor);
}

LLTexUnit* LLRender::getTexUnit(U32 index)
{
    if (index < mTexUnits.size())
    {
        return &mTexUnits[index];
    }
    else
    {
        LL_DEBUGS() << "Non-existing texture unit layer requested: " << index << LL_ENDL;
        return &mDummyTexUnit;
    }
}

void LLRender::begin(const U32& mode)
{
    mMode = mode;
    sBackend->begin(mode);
}

void LLRender::end()
{
    sBackend->end();
}

void LLRender::flush()
{
    sBackend->flush();
}

void LLRender::setMinimumAlpha(F32 minimum)
{
    sBackend->setMinimumAlpha(minimum);
}

void LLRender::vertex3f(const F32& x, const F32& y, const F32& z)
{
    if (sBackendCapture) sBackendCaptureIncomplete = true;
    // Apply the UI offset/scale stacks (translateUI/scaleUI), then forward.
    if (mUIOffset.empty())
    {
        sBackend->vertex3f(x, y, z);
    }
    else
    {
        LLVector4a vert(x, y, z);
        vert.add(mUIOffset.back());
        vert.mul(mUIScale.back());
        sBackend->vertex3f(vert[0], vert[1], vert[2]);
    }
}

void LLRender::vertexBatchPreTransformed(LLVector4a* verts, S32 vert_count)
{
    if (sBackendCapture) sBackendCaptureIncomplete = true;
    // No UVs/colors supplied: forward positions with the current color (the facade
    // carries current color state).
    for (S32 i = 0; i < vert_count; ++i)
    {
        sBackend->vertex3f(verts[i][0], verts[i][1], verts[i][2]);
    }
}

void LLRender::vertexBatchPreTransformed(LLVector4a* verts, LLVector2* uvs, S32 vert_count)
{
    if (sBackendCapture) sBackendCaptureIncomplete = true;
    for (S32 i = 0; i < vert_count; ++i)
    {
        sBackend->texCoord2f(uvs[i].mV[0], uvs[i].mV[1]);
        sBackend->vertex3f(verts[i][0], verts[i][1], verts[i][2]);
    }
}

void LLRender::vertexBatchPreTransformed(LLVector4a* verts, LLVector2* uvs, LLColor4U* colors, S32 vert_count)
{
    {
        // The exact call LLFontGL uses for glyph quads. Pack the three parallel
        // arrays into the backend's flat pos(xyz)/uv(uv)/col(rgba8) form. mUIOffset
        // is NOT applied: these are already screen-space.
        std::vector<F32> pos((size_t)vert_count * 3);
        std::vector<F32> uv((size_t)vert_count * 2);
        std::vector<U8>  col((size_t)vert_count * 4);
        for (S32 i = 0; i < vert_count; ++i)
        {
            pos[i * 3 + 0] = verts[i][0];
            pos[i * 3 + 1] = verts[i][1];
            pos[i * 3 + 2] = verts[i][2];
            uv[i * 2 + 0] = uvs[i].mV[0];
            uv[i * 2 + 1] = uvs[i].mV[1];
            col[i * 4 + 0] = colors[i].mV[0];
            col[i * 4 + 1] = colors[i].mV[1];
            col[i * 4 + 2] = colors[i].mV[2];
            col[i * 4 + 3] = colors[i].mV[3];
        }
        sBackend->vertexBatchPreTransformed(pos.data(), uv.data(), col.data(),
                                            vert_count);
        if (sBackendCapture)
        {
            LLRenderCapturedBatch b;
            b.mMode = mMode;
            b.mImage = sBackendBoundImage0;
            b.mPos = std::move(pos); b.mUV = std::move(uv); b.mCol = std::move(col);
            sBackendCapture->push_back(std::move(b));
        }
    }
}

void LLRender::vertex2i(const S32& x, const S32& y)
{
    vertex3f((F32) x, (F32) y, 0);
}

void LLRender::vertex2f(const F32& x, const F32& y)
{
    vertex3f(x,y,0);
}

void LLRender::vertex2fv(const F32* v)
{
    vertex3f(v[0], v[1], 0);
}

void LLRender::vertex3fv(const F32* v)
{
    vertex3f(v[0], v[1], v[2]);
}

void LLRender::texCoord2f(const F32& x, const F32& y)
{
    sBackend->texCoord2f(x, y);
}

void LLRender::texCoord2i(const S32& x, const S32& y)
{
    texCoord2f((F32) x, (F32) y);
}

void LLRender::texCoord2fv(const F32* tc)
{
    texCoord2f(tc[0], tc[1]);
}

void LLRender::color4ub(const U8& r, const U8& g, const U8& b, const U8& a)
{
    sBackend->color4ub(r, g, b, a);
}

void LLRender::color4ubv(const U8* c)
{
    color4ub(c[0], c[1], c[2], c[3]);
}

void LLRender::color4f(const F32& r, const F32& g, const F32& b, const F32& a)
{
    sBackend->color4f(r, g, b, a);
}

void LLRender::color4fv(const F32* c)
{
    color4f(c[0],c[1],c[2],c[3]);
}

void LLRender::color3f(const F32& r, const F32& g, const F32& b)
{
    color4f(r,g,b,1);
}

void LLRender::color3fv(const F32* c)
{
    color4f(c[0],c[1],c[2],1);
}

void LLRender::setLineWidth(F32 width)
{
    sBackend->setLineWidth(width);
}

void LLRender::setLabelAnchor(F32 x, F32 y, F32 z, F32 bias)
{
    sBackend->setLabelAnchor(true, x, y, z, bias);
}

void LLRender::clearLabelAnchor()
{
    sBackend->setLabelAnchor(false, 0.f, 0.f, 0.f, 0.f);
}

///////////////////////////////////////////////////////////////
//
// Render-state scopes
//

bool LLRender::sBlendEnabled = false;
bool LLRender::sCullFaceEnabled = false;
bool LLRender::sScissorEnabled = false;
bool LLRender::sDepthEnabled = false;
LLDepthFunc LLRender::sDepthFunc = LLDepthFunc::LESS;
bool LLRender::sDepthWriteEnabled = true;

// static
bool& LLRender::stateFlag(LLRenderState state)
{
    switch (state)
    {
        case LLRenderState::BLEND:        return sBlendEnabled;
        case LLRenderState::CULL_FACE:    return sCullFaceEnabled;
        case LLRenderState::SCISSOR_TEST: return sScissorEnabled;
        default: break;
    }
    llassert(false);
    static bool s_none = false;
    s_none = false;
    return s_none;
}

// static
bool LLRender::isStateEnabled(LLRenderState state)
{
    return state != LLRenderState::NONE && stateFlag(state);
}

LLRender::StateScope::StateScope(LLRenderState state, bool enable)
    : mState(state)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    if (mState == LLRenderState::NONE)
    {
        return;
    }
    bool& flag = stateFlag(mState);
    mWasEnabled = flag;
    if (flag != enable)
    {
        gGL.flush();
        flag = enable;
    }
    mIsEnabled = enable;
}

LLRender::StateScope::~StateScope()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    if (mState != LLRenderState::NONE && mIsEnabled != mWasEnabled)
    {
        gGL.flush();
        stateFlag(mState) = mWasEnabled;
    }
}

// Push the current depth state to the backend (LLDepthFunc -> LLRender compare
// func). The backend only honours it inside the world-overlay segment (property
// lines, edit manipulators, beacons), which has the world depth attached; the 2D UI
// layer always renders with depth off.
// static
void LLRender::forwardDepthState()
{
    U32 f = CF_LESS_EQUAL;
    switch (sDepthFunc)
    {
        case LLDepthFunc::NEVER:    f = CF_NEVER; break;
        case LLDepthFunc::ALWAYS:   f = CF_ALWAYS; break;
        case LLDepthFunc::LESS:     f = CF_LESS; break;
        case LLDepthFunc::LEQUAL:   f = CF_LESS_EQUAL; break;
        case LLDepthFunc::EQUAL:    f = CF_EQUAL; break;
        case LLDepthFunc::NOTEQUAL: f = CF_NOT_EQUAL; break;
        case LLDepthFunc::GEQUAL:   f = CF_GREATER_EQUAL; break;
        case LLDepthFunc::GREATER:  f = CF_GREATER; break;
        default: break;
    }
    sBackend->setDepthTest(sDepthEnabled, sDepthWriteEnabled, f);
}

LLRender::DepthScope::DepthScope(bool depth_enabled, bool write_enabled, LLDepthFunc depth_func)
    : mPrevDepthEnabled(sDepthEnabled), mPrevDepthFunc(sDepthFunc), mPrevWriteEnabled(sDepthWriteEnabled)
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;

    if (!depth_enabled)
    {
        // Depth writes without testing are not a thing: writing depth unconditionally
        // is test on + LLDepthFunc::ALWAYS.
        write_enabled = false;
    }

    if (depth_enabled != sDepthEnabled)
    {
        gGL.flush();
        sDepthEnabled = depth_enabled;
    }
    if (depth_func != sDepthFunc)
    {
        gGL.flush();
        sDepthFunc = depth_func;
    }
    if (write_enabled != sDepthWriteEnabled)
    {
        gGL.flush();
        sDepthWriteEnabled = write_enabled;
    }
    forwardDepthState();
}

LLRender::DepthScope::~DepthScope()
{
    LL_PROFILE_ZONE_SCOPED_CATEGORY_PIPELINE;
    if (sDepthEnabled != mPrevDepthEnabled)
    {
        gGL.flush();
        sDepthEnabled = mPrevDepthEnabled;
    }
    if (sDepthFunc != mPrevDepthFunc)
    {
        gGL.flush();
        sDepthFunc = mPrevDepthFunc;
    }
    if (sDepthWriteEnabled != mPrevWriteEnabled)
    {
        gGL.flush();
        sDepthWriteEnabled = mPrevWriteEnabled;
    }
    forwardDepthState();
}

glm::mat4 get_current_modelview()
{
    return glm::make_mat4(gGLModelView);
}

glm::mat4 get_current_projection()
{
    return glm::make_mat4(gGLProjection);
}

glm::mat4 get_last_modelview()
{
    return glm::make_mat4(gGLLastModelView);
}

glm::mat4 get_last_projection()
{
    return glm::make_mat4(gGLLastProjection);
}

void copy_matrix(const glm::mat4& src, F32* dst)
{
    auto matp = glm::value_ptr(src);
    for (U32 i = 0; i < 16; i++)
    {
        dst[i] = matp[i];
    }
}

void set_current_modelview(const glm::mat4& mat)
{
    copy_matrix(mat, gGLModelView);
}

void set_current_projection(const glm::mat4& mat)
{
    copy_matrix(mat, gGLProjection);
}

void set_last_modelview(const glm::mat4& mat)
{
    copy_matrix(mat, gGLLastModelView);
}

void set_last_projection(const glm::mat4& mat)
{
    copy_matrix(mat, gGLLastProjection);
}

glm::vec3 mul_mat4_vec3(const glm::mat4& mat, const glm::vec3& vec)
{
#if 1 // SIMD path results in strange crashes. Fall back to scalar for now.
    const float w = vec[0] * mat[0][3] + vec[1] * mat[1][3] + vec[2] * mat[2][3] + mat[3][3];
    return glm::vec3(
       (vec[0] * mat[0][0] + vec[1] * mat[1][0] + vec[2] * mat[2][0] + mat[3][0]) / w,
       (vec[0] * mat[0][1] + vec[1] * mat[1][1] + vec[2] * mat[2][1] + mat[3][1]) / w,
       (vec[0] * mat[0][2] + vec[1] * mat[1][2] + vec[2] * mat[2][2] + mat[3][2]) / w
    );
#else
    LLVector4a x, y, z, s, t, p, q;

    x.splat(vec.x);
    y.splat(vec.y);
    z.splat(vec.z);

    s.splat<3>(mat[0].data);
    t.splat<3>(mat[1].data);
    p.splat<3>(mat[2].data);
    q.splat<3>(mat[3].data);

    s.mul(x);
    t.mul(y);
    p.mul(z);
    q.add(s);
    t.add(p);
    q.add(t);

    x.mul(mat[0].data);
    y.mul(mat[1].data);
    z.mul(mat[2].data);

    x.add(y);
    z.add(mat[3].data);
    LLVector4a res;
    res.load3(glm::value_ptr(vec));
    res.setAdd(x, z);
    res.div(q);
    return glm::make_vec3(res.getF32ptr());
#endif
}
