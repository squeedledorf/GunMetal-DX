/**
 * @file llrender.h
 * @brief LLRender definition
 *
 *  Immediate-mode UI drawing API (gGL). Every call forwards to the installed
 *  LLRenderBackend (the Vulkan UI facade, see llrenderbackend.h); until it is
 *  installed (and after teardown) a no-op backend stands in.
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

#ifndef LL_LLGLRENDER_H
#define LL_LLGLRENDER_H
#include <functional>

#include "v2math.h"
#include "v3math.h"
#include "v4coloru.h"
#include "v4math.h"
#include "llstrider.h"
#include "llpointer.h"
#include <vector>
#include "llwin32headers.h"
#include "llpixelformat.h"
#include "llmatrix4a.h"
#include "glm/mat4x4.hpp"
#include <boost/align/aligned_allocator.hpp>

#include <array>
#include <list>

class LLTextureData;
class LLTexture;
class LLRenderBackend;

constexpr U32 LL_NUM_TEXTURE_LAYERS = 32;

class LLTexUnit
{
    friend class LLRender;
public:
    typedef enum
    {
        TT_TEXTURE = 0,         // Standard 2D Texture
        TT_RECT_TEXTURE,        // Non power of 2 texture
        TT_CUBE_MAP,            // 6-sided cube map texture
        TT_CUBE_MAP_ARRAY,      // Array of cube maps
        TT_MULTISAMPLE_TEXTURE, // see GL_ARB_texture_multisample
        TT_TEXTURE_3D,          // standard 3D Texture
        TT_NONE,                // No texture type is currently enabled
    } eTextureType;

    typedef enum
    {
        TAM_WRAP = 0,
        TAM_MIRROR,
        TAM_CLAMP
    } eTextureAddressMode;

    typedef enum
    {
        TFO_POINT = 0,          // Equal to: min=point, mag=point, mip=none.
        TFO_BILINEAR,           // Equal to: min=linear, mag=linear, mip=point.
        TFO_TRILINEAR,          // Equal to: min=linear, mag=linear, mip=linear.
        TFO_ANISOTROPIC         // Equal to: min=anisotropic, max=anisotropic, mip=linear.
    } eTextureFilterOptions;

    typedef enum
    {
        TMG_NONE = 0,           // Mipmaps are not automatically generated for this texture.
        TMG_AUTO,               // Mipmaps are automatically generated for this texture.
        TMG_MANUAL              // Mipmaps are manually generated for this texture.
    } eTextureMipGeneration;

    LLTexUnit(S32 index = -1);

    // returns the index of this texture unit
    S32 getIndex(void) const { return mIndex; }

    // Enables this texture unit for the given texture type
    // (automatically disables any previously enabled texture type)
    void enable(eTextureType type);

    // Disables the current texture unit (unbinds what it holds)
    void disable(void);

    // Binds the LLTextureData to this texture unit via the backend
    bool bind(LLTextureData* texture, bool for_rendering = false, bool forceBind = false, S32 usename = 0);
    bool bind(LLTexture* texture, bool for_rendering = false, bool forceBind = false);

    // Unbinds the currently bound texture of the given type
    void unbind(eTextureType type);

    // Sets the addressing mode used to sample the texture bound to this unit
    // (recorded on the image, so a later re-bind keeps it).
    void setTextureAddressMode(eTextureAddressMode mode);

    // Sets the filtering options used to sample the texture bound to this unit
    void setTextureFilteringOption(LLTexUnit::eTextureFilterOptions option);

    eTextureType getCurrType(void) { return mCurrTexType; }

protected:
    friend class LLRender;

    S32                 mIndex;
    LLTextureData*          mVKBound = nullptr;   // image bound on this unit (address mode target)
    eTextureType        mCurrTexType;
};

// Backend capture of pre-transformed glyph batches, so LLFontVertexBuffer can cache
// laid-out text and replay it without re-running glyph layout.
struct LLRenderCapturedBatch
{
    U32 mMode = 0;
    LLPointer<LLTextureData> mImage;
    std::vector<F32> mPos;   // xyz per vertex
    std::vector<F32> mUV;    // uv per vertex
    std::vector<U8>  mCol;   // rgba8 per vertex
};

class LLRender
{
    friend class LLTexUnit;
public:

    enum eTexIndex : U8
    {
        // Channels for material textures
        DIFFUSE_MAP            = 0,
        ALTERNATE_DIFFUSE_MAP  = 1,
        NORMAL_MAP             = 1,
        SPECULAR_MAP           = 2,
        // Channels for PBR textures
        BASECOLOR_MAP          = 3,
        METALLIC_ROUGHNESS_MAP = 4,
        GLTF_NORMAL_MAP        = 5,
        EMISSIVE_MAP           = 6,
        // Total number of channels
        NUM_TEXTURE_CHANNELS   = 7,
    };

    enum eVolumeTexIndex : U8
    {
        LIGHT_TEX = 0,
        SCULPT_TEX,
        NUM_VOLUME_TEXTURE_CHANNELS,
    };

    enum eGeomModes : U8
    {
        TRIANGLES = 0,
        TRIANGLE_STRIP,
        TRIANGLE_FAN,
        POINTS,
        LINES,
        LINE_STRIP,
        LINE_LOOP,
        NUM_MODES
    };

    enum eCompareFunc : U8
    {
        CF_NEVER = 0,
        CF_ALWAYS,
        CF_LESS,
        CF_LESS_EQUAL,
        CF_EQUAL,
        CF_NOT_EQUAL,
        CF_GREATER_EQUAL,
        CF_GREATER,
        CF_DEFAULT
    };

    enum eBlendType : U8
    {
        BT_ALPHA = 0,
        BT_ADD,
        BT_ADD_WITH_ALPHA,  // Additive blend modulated by the fragment's alpha.
        BT_MULT,
        BT_MULT_ALPHA,
        BT_MULT_X2,
        BT_REPLACE
    };

    // WARNING:  this MUST match the LL_PART_BF enum in LLPartData, so set values explicitly in case someone
    // decides to add more or reorder them
    enum eBlendFactor : U8
    {
        BF_ONE = 0,
        BF_ZERO = 1,
        BF_DEST_COLOR = 2,
        BF_SOURCE_COLOR = 3,
        BF_ONE_MINUS_DEST_COLOR = 4,
        BF_ONE_MINUS_SOURCE_COLOR = 5,
        BF_DEST_ALPHA = 6,
        BF_SOURCE_ALPHA = 7,
        BF_ONE_MINUS_DEST_ALPHA = 8,
        BF_ONE_MINUS_SOURCE_ALPHA = 9,
        BF_UNDEF
    };

    enum eMatrixMode : U8
    {
        MM_MODELVIEW = 0,
        MM_PROJECTION,
        MM_TEXTURE0,
        MM_TEXTURE1,
        MM_TEXTURE2,
        MM_TEXTURE3,
        NUM_MATRIX_MODES,
        MM_TEXTURE
    };

    // The backend every immediate-mode + render-state method below dispatches to.
    // Never null: newview installs the Vulkan UI backend
    // (LLVulkanContext::initRenderBackend); before that and after teardown it points
    // at a no-op backend. See llrenderbackend.h.
    static LLRenderBackend* sBackend;
    // True while a real backend is installed (setBackend with non-null).
    static bool hasBackend() { return sBackendInstalled; }
    // Backend glyph-batch capture (see LLRenderCapturedBatch). end returns false if
    // anything other than pre-transformed colored batches was drawn meanwhile (the
    // capture can't reproduce it; the caller re-lays-out instead).
    static void beginBackendCapture(std::vector<LLRenderCapturedBatch>* out);
    static bool endBackendCapture();
    static void replayBackendCapture(const std::vector<LLRenderCapturedBatch>& batches);
    // RETAINED UI (llrenderrecorder.h): record everything sent to the backend between
    // begin/end into `out` (while still drawing it); replay re-issues a recording and
    // restores LLRender's texture bookkeeping to what the recorded draw left behind.
    // beginRetainedCapture returns false (nothing installed) without a backend or when
    // a capture is already running; endRetainedCapture returns whether it is usable.
    static bool beginRetainedCapture(class LLRenderRecording* out);
    static bool endRetainedCapture();
    static void replayRetained(const class LLRenderRecording& rec, const std::function<void(U32)>& live = nullptr);
    // Live sections inside a recording (a view whose content changes every frame):
    // retainedCaptureActive() is true while recording and not paused; recordRetainedLive
    // inserts a marker the replay calls back on; pause stops recording the live draw.
    static bool retainedCaptureActive();
    static void recordRetainedLive(U32 index);
    static void pauseRetainedCapture(bool pause);
    static std::vector<LLRenderCapturedBatch>* sBackendCapture;
    static bool sBackendCaptureIncomplete;
    static LLTextureData* sBackendBoundImage0;   // unit-0 image as last bound via LLTexUnit
    // Install the backend; nullptr restores the no-op backend.
    static void setBackend(LLRenderBackend* backend);

    LLRender();
    ~LLRender();

    void translatef(const F32& x, const F32& y, const F32& z);
    void scalef(const F32& x, const F32& y, const F32& z);
    void rotatef(const F32& a, const F32& x, const F32& y, const F32& z);
    void ortho(F32 left, F32 right, F32 bottom, F32 top, F32 zNear, F32 zFar);

    void pushMatrix();
    void popMatrix();
    void loadMatrix(const F32* m);
    void loadIdentity();
    void multMatrix(const F32* m);
    void matrixMode(eMatrixMode mode);
    eMatrixMode getMatrixMode();

    void translateUI(F32 x, F32 y, F32 z);
    void scaleUI(F32 x, F32 y, F32 z);
    void pushUIMatrix();
    void popUIMatrix();
    void loadUIIdentity();
    LLVector3 getUITranslation();
    LLVector3 getUIScale();

    void flush();
    // Alpha test threshold for subsequent draws; the backend honours it inside
    // offscreen render targets only (#83: the avatar compositor).
    void setMinimumAlpha(F32 minimum);

    void begin(const U32& mode);
    void end();

    U8 getMode() const { return mMode; }

    void vertex2i(const S32& x, const S32& y);
    void vertex2f(const F32& x, const F32& y);
    void vertex3f(const F32& x, const F32& y, const F32& z);
    void vertex2fv(const F32* v);
    void vertex3fv(const F32* v);

    void texCoord2i(const S32& x, const S32& y);
    void texCoord2f(const F32& x, const F32& y);
    void texCoord2fv(const F32* tc);

    void color4ub(const U8& r, const U8& g, const U8& b, const U8& a);
    void color4f(const F32& r, const F32& g, const F32& b, const F32& a);
    void color4fv(const F32* c);
    void color3f(const F32& r, const F32& g, const F32& b);
    void color3fv(const F32* c);
    void color4ubv(const U8* c);

    void vertexBatchPreTransformed(LLVector4a* verts, S32 vert_count);
    void vertexBatchPreTransformed(LLVector4a* verts, LLVector2* uvs, S32 vert_count);
    void vertexBatchPreTransformed(LLVector4a* verts, LLVector2* uvs, LLColor4U*, S32 vert_count);

    void setColorMask(bool writeColor, bool writeAlpha);
    void setColorMask(bool writeColorR, bool writeColorG, bool writeColorB, bool writeAlpha);
    void setSceneBlendType(eBlendType type);

    // applies blend func to both color and alpha
    void blendFunc(eBlendFactor sfactor, eBlendFactor dfactor);
    // separate color/alpha blend functions; the backend applies the color pair to both
    void blendFunc(eBlendFactor color_sfactor, eBlendFactor color_dfactor,
               eBlendFactor alpha_sfactor, eBlendFactor alpha_dfactor);

    LLTexUnit* getTexUnit(U32 index);

    U32 getCurrentTexUnitIndex(void) const { return 0; }

    void setLineWidth(F32 width);

    // In-world label occlusion (#487): name tags / hover text set the agent-space
    // point they are attached to before drawing (with a DepthScope testing) and clear it
    // after; the backend hides or fades the whole label when the scene covers that
    // point, instead of depth-testing each glyph.
    void setLabelAnchor(F32 x, F32 y, F32 z, F32 bias = 0.15f);
    void clearLabelAnchor();

    // ---- render-state scopes (RAII) ------------------------------------------
    // Each scope sets a piece of render state for its lifetime and restores the
    // previous value on exit; scopes nest. The state is process-wide (static, like
    // sBackend). Every effective change flushes the immediate-mode batch, so batches
    // never straddle a state boundary.
    //
    // Blend / cull / scissor are tracked on the CPU only: the backend picks blending
    // per draw (setSceneBlendType / blendFunc), never culls immediate-mode geometry,
    // and gets its scissor from LLScreenClipRect. Depth is forwarded to the backend
    // (setDepthTest), which honours it inside the world-overlay segment.

    // Toggle one LLRenderState. LLRenderState::NONE makes a no-op scope.
    class StateScope
    {
    public:
        StateScope(LLRenderState state, bool enable);
        ~StateScope();
        StateScope(const StateScope&) = delete;
        StateScope& operator=(const StateScope&) = delete;
    private:
        LLRenderState mState;
        bool mWasEnabled = false;
        bool mIsEnabled = false;
    };

    struct BlendScope : public StateScope
    {
        explicit BlendScope(bool enable = true) : StateScope(LLRenderState::BLEND, enable) {}
    };

    struct CullScope : public StateScope
    {
        explicit CullScope(bool enable = true) : StateScope(LLRenderState::CULL_FACE, enable) {}
    };

    struct ScissorScope : public StateScope
    {
        explicit ScissorScope(bool enable = true) : StateScope(LLRenderState::SCISSOR_TEST, enable) {}
    };

    // Depth test / write / compare function. Disabling the test also disables
    // writes (writing depth without testing takes test on + LLDepthFunc::ALWAYS).
    class DepthScope
    {
    public:
        DepthScope(bool depth_enabled, bool write_enabled = true, LLDepthFunc depth_func = LLDepthFunc::LEQUAL);
        ~DepthScope();
        DepthScope(const DepthScope&) = delete;
        DepthScope& operator=(const DepthScope&) = delete;
    private:
        bool mPrevDepthEnabled;
        LLDepthFunc mPrevDepthFunc;
        bool mPrevWriteEnabled;
    };

    // The 2D UI default: blend on, cull off, depth test off.
    class UIScope
    {
    public:
        UIScope() = default;
        UIScope(const UIScope&) = delete;
        UIScope& operator=(const UIScope&) = delete;
    private:
        // Construction order (blend, cull, depth) is the order the state is set in;
        // destruction restores it in reverse.
        BlendScope mBlend{ true };
        CullScope mCullFace{ false };
        DepthScope mDepthTest{ false, true, LLDepthFunc::LEQUAL };
    };

    static bool isStateEnabled(LLRenderState state);

public:
    static LLVector2 sUIGLScaleFactor;


private:
    static bool sBackendInstalled;

    static bool& stateFlag(LLRenderState state);
    static void forwardDepthState();

    static bool sBlendEnabled;
    static bool sCullFaceEnabled;
    static bool sScissorEnabled;
    static bool sDepthEnabled;
    static LLDepthFunc sDepthFunc;
    static bool sDepthWriteEnabled;

    eMatrixMode mMatrixMode;
    U32             mMode;
    std::array<LLTexUnit, LL_NUM_TEXTURE_LAYERS> mTexUnits;
    LLTexUnit           mDummyTexUnit;

    std::vector<LLVector4a, boost::alignment::aligned_allocator<LLVector4a, 16> > mUIOffset;
    std::vector<LLVector4a, boost::alignment::aligned_allocator<LLVector4a, 16> > mUIScale;
};

extern F32 gGLModelView[16];
extern F32 gGLLastModelView[16];
extern F32 gGLLastProjection[16];
extern F32 gGLProjection[16];
extern S32 gGLViewport[4];
extern glm::mat4 gGLDeltaModelView;
extern glm::mat4 gGLInverseDeltaModelView;

extern thread_local LLRender gGL;

// This rotation matrix moves the default OpenGL reference frame
// (-Z at, Y up) to Cory's favorite reference frame (X at, Z up)
const F32 OGL_TO_CFR_ROTATION[16] = {  0.f,  0.f, -1.f,  0.f,   // -Z becomes X
                                      -1.f,  0.f,  0.f,  0.f,   // -X becomes Y
                                       0.f,  1.f,  0.f,  0.f,   //  Y becomes Z
                                       0.f,  0.f,  0.f,  1.f };

glm::mat4 copy_matrix(F32* src);
glm::mat4 get_current_modelview();
glm::mat4 get_current_projection();
glm::mat4 get_last_modelview();
glm::mat4 get_last_projection();

void copy_matrix(const glm::mat4& src, F32* dst);
void set_current_modelview(const glm::mat4& mat);
void set_current_projection(const glm::mat4& mat);
void set_last_modelview(const glm::mat4& mat);
void set_last_projection(const glm::mat4& mat);

// glh compat
glm::vec3 mul_mat4_vec3(const glm::mat4& mat, const glm::vec3& vec);

#define LL_SHADER_LOADING_WARNS(...) LL_WARNS()

#endif
