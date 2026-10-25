/**
 *
 * Copyright (c) 2021, Kitty Barnett
 *
 * The source code in this file is provided to you under the terms of the
 * GNU Lesser General Public License, version 2.1, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A
 * PARTICULAR PURPOSE. Terms of the LGPL can be found in doc/LGPL-licence.txt
 * in this distribution, or online at http://www.gnu.org/licenses/lgpl-2.1.txt
 *
 * By copying, modifying or distributing this software, you acknowledge that
 * you have read and understood your obligations described above, and agree to
 * abide by those obligations.
 *
 */

#include "llviewerprecompiledheaders.h"

#include "llagent.h"
#include "llfasttimer.h"
#include "llviewercamera.h"
#include "llrender.h"
#include "llviewertexturelist.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"

#include "rlveffects.h"
#include "rlvhandler.h"

// ====================================================================================
// RlvOverlayEffect class
//

const float c_DefaultAlpha = 1.0f;
const float c_DefaultColor[3] = { 1.0f, 1.0f, 1.0f };

RlvOverlayEffect::RlvOverlayEffect(const LLUUID& idRlvObj)
    : LLVisualEffect(idRlvObj, EVisualEffect::RlvOverlay, EVisualEffectType::Custom)
    , m_nAlpha(c_DefaultAlpha)
    , m_fBlockTouch(false)
    , m_Color(LLColor3(c_DefaultColor))
{
    m_fEnabled = false;
}

RlvOverlayEffect::~RlvOverlayEffect()
{
    clearImage();
}

// static
ERlvCmdRet RlvOverlayEffect::onAlphaValueChanged(const LLUUID& idRlvObj, const boost::optional<RlvBehaviourModifierValue> newValue)
{
    if (RlvOverlayEffect* pEffect = LLVfxManager::instance().getEffect<RlvOverlayEffect>(idRlvObj))
    {
        pEffect->m_nAlpha = (newValue) ? boost::get<float>(newValue.value()) : c_DefaultAlpha;
    }
    return RLV_RET_SUCCESS;
}

// static
ERlvCmdRet RlvOverlayEffect::onColorValueChanged(const LLUUID& idRlvObj, const boost::optional<RlvBehaviourModifierValue> newValue)
{
    if (RlvOverlayEffect* pEffect = LLVfxManager::instance().getEffect<RlvOverlayEffect>(idRlvObj))
    {
        pEffect->m_Color = LLColor3( (newValue) ? boost::get<LLVector3>(newValue.value()).mV : c_DefaultColor);
    }
    return RLV_RET_SUCCESS;
}

// static
ERlvCmdRet RlvOverlayEffect::onTextureChanged(const LLUUID& idRlvObj, const boost::optional<RlvBehaviourModifierValue> newValue)
{
    if (RlvOverlayEffect* pEffect = LLVfxManager::instance().getEffect<RlvOverlayEffect>(idRlvObj))
    {
        if (newValue)
            pEffect->setImage(boost::get<LLUUID>(newValue.value()));
        else
            pEffect->clearImage();
    }
    return RLV_RET_SUCCESS;
}

void RlvOverlayEffect::clearImage()
{
    if (m_pImage)
    {
        m_pImage->setBoostLevel(m_nImageOrigBoost);
        m_pImage = nullptr;
    }
    m_Mask.clear();
    m_nMaskWidth = m_nMaskHeight = 0;
    m_nMaskDiscard = -1;
    m_fMaskOpaque = false;
}

// (#704) The overlay's own mask from the saved raw image; no image data yet = solid.
bool RlvOverlayEffect::maskAt(float u, float v) const
{
    if (m_pImage->hasSavedRawImage() && m_pImage->getSavedRawImageLevel() != m_nMaskDiscard)
    {
        const LLImageRaw* raw = m_pImage->getSavedRawImage();
        const S32 w = raw ? raw->getWidth() : 0, h = raw ? raw->getHeight() : 0, c = raw ? raw->getComponents() : 0;
        const U8* data = raw ? raw->getData() : nullptr;
        if (data && w > 0 && h > 0 && c > 0)
        {
            m_nMaskDiscard = m_pImage->getSavedRawImageLevel();
            m_fMaskOpaque = c != 2 && c != 4;   // no alpha channel
            m_Mask.clear();
            m_nMaskWidth = m_nMaskHeight = 0;
            if (!m_fMaskOpaque)
            {
                constexpr S32 kMaxDim = 128;
                const S32 step = llmax(1, (llmax(w, h) + kMaxDim - 1) / kMaxDim);
                const S32 mw = (w + step - 1) / step, mh = (h + step - 1) / step;
                m_Mask.assign((size_t)mw * mh, 0);
                for (S32 my = 0; my < mh; ++my)
                {
                    for (S32 mx = 0; mx < mw; ++mx)
                    {
                        U8 solid = 0;
                        for (S32 y = my * step; y < llmin(h, (my + 1) * step) && !solid; ++y)
                        {
                            for (S32 x = mx * step; x < llmin(w, (mx + 1) * step); ++x)
                            {
                                if (data[((size_t)y * w + x) * c + (c - 1)] > 32)
                                {
                                    solid = 1;
                                    break;
                                }
                            }
                        }
                        m_Mask[(size_t)my * mw + mx] = solid;
                    }
                }
                m_nMaskWidth = (U16)mw;
                m_nMaskHeight = (U16)mh;
            }
        }
    }
    if (m_fMaskOpaque || m_Mask.empty() || !m_nMaskWidth || !m_nMaskHeight)
        return true;
    const float fu = u - floorf(u), fv = v - floorf(v);
    const S32 x = llclamp((S32)(fu * (float)m_nMaskWidth), 0, (S32)m_nMaskWidth - 1);
    const S32 y = llclamp((S32)(fv * (float)m_nMaskHeight), 0, (S32)m_nMaskHeight - 1);
    return m_Mask[(size_t)y * m_nMaskWidth + x] != 0;
}

bool RlvOverlayEffect::hitTest(const LLCoordGL& ptMouse) const
{
    if (!m_pImage)
        return false;

    return (m_fBlockTouch) && maskAt((float)ptMouse.mX / gViewerWindow->getWorldViewWidthScaled(), (float)ptMouse.mY / gViewerWindow->getWorldViewHeightScaled());
}

void RlvOverlayEffect::setImage(const LLUUID& idTexture)
{
    if ( (m_pImage) && (m_pImage->getID() == idTexture) )
        return;

    clearImage();
    m_pImage = LLViewerTextureManager::getFetchedTexture(idTexture, FTT_DEFAULT, MIPMAP_YES, LLGLTexture::BOOST_NONE, LLViewerTexture::LOD_TEXTURE);
    m_nImageOrigBoost = m_pImage->getBoostLevel();
    m_pImage->setBoostLevel(LLGLTexture::BOOST_PREVIEW);
    m_pImage->forceToSaveRawImage(0);
}

void RlvOverlayEffect::run(const LLVisualEffectParams*)
{
    if (m_pImage)
    {
        gGL.flush();

        int nWidth = gViewerWindow->getWorldViewWidthScaled();
        int nHeight = gViewerWindow->getWorldViewHeightScaled();

        m_pImage->addTextureStats((F32)(nWidth * nHeight));
        m_pImage->setKnownDrawSize(nWidth, nHeight);

        gGL.pushMatrix();
        LLRender::UIScope glsUI;
        gViewerWindow->setup2DRender();

        const LLVector2& displayScale = gViewerWindow->getDisplayScale();
        gGL.scalef(displayScale.mV[VX], displayScale.mV[VY], 1.f);

        gGL.getTexUnit(0)->bind(m_pImage);
        const LLColor3 col = m_Color.get();
        gGL.color4f(col.mV[0], col.mV[1], col.mV[2], llclamp(m_nAlpha.get(), 0.0f, 1.0f));

        gl_rect_2d_simple_tex(nWidth, nHeight);

        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);

        gGL.popMatrix();
        gGL.flush();
        gViewerWindow->setup3DRender();

        gGL.flush();
    }
}

// ====================================================================================
// RlvSphereEffect class
//

const int   c_SphereDefaultMode = 0;
const int   c_SphereDefaultOrigin = 0;
const float c_SphereDefaultColor[4] = { 0.0, 0.f, 0.f, 0.f };
const float c_SphereDefaultDistance = 0.0f;
const int   c_SphereDefaultDistanceExtend = 1;
const float c_SphereDefaultAlpha = 1.0f;

RlvSphereEffect::RlvSphereEffect(const LLUUID& idRlvObj)
    : LLVisualEffect(idRlvObj, EVisualEffect::RlvSphere, EVisualEffectType::PostProcessShader)
    , m_eMode((ESphereMode)c_SphereDefaultMode)
    , m_eOrigin((ESphereOrigin)c_SphereDefaultOrigin)
    , m_Params(LLVector4(c_SphereDefaultColor))
    , m_nDistanceMin(c_SphereDefaultDistance), m_nDistanceMax(c_SphereDefaultDistance)
    , m_eDistExtend((ESphereDistExtend)c_SphereDefaultDistanceExtend)
    , m_nValueMin(c_SphereDefaultAlpha), m_nValueMax(c_SphereDefaultAlpha)
    , m_nTweenDuration(0.f)
{
}

RlvSphereEffect::~RlvSphereEffect()
{
}

void RlvSphereEffect::run(const LLVisualEffectParams* pParams)
{
    RlvSphereRenderParams* pOut = const_cast<RlvSphereRenderParams*>(dynamic_cast<const RlvSphereRenderParams*>(pParams));
    if (!pOut || pOut->count >= RlvSphereRenderParams::MAX_SPHERES)
        return;

    RlvSphereRenderParams::Sphere& sphere = pOut->spheres[pOut->count++];
    sphere.mode = llclamp((int)m_eMode, 0, (int)ESphereMode::Count);
    const LLVector3 posOrigin = (ESphereOrigin::Camera == m_eOrigin) ? LLViewerCamera::instance().getOrigin()
                              : (isAgentAvatarValid()) ? gAgentAvatarp->getRenderPosition() : gAgent.getPositionAgent();
    sphere.origin[0] = posOrigin.mV[VX]; sphere.origin[1] = posOrigin.mV[VY]; sphere.origin[2] = posOrigin.mV[VZ];
    const float nDistMin = m_nDistanceMin.get(), nDistMax = m_nDistanceMax.get();
    sphere.values[0] = m_nValueMin.get();
    sphere.values[1] = nDistMin;
    sphere.values[2] = m_nValueMax.get();
    sphere.values[3] = (nDistMax >= nDistMin) ? nDistMax : nDistMin;
    const LLVector4 params = m_Params.get();
    for (int i = 0; i < 4; ++i)
        sphere.params[i] = params.mV[i];
    // Script-chosen numbers reach the shader as they are: no NaN / inf, nothing absurd.
    auto sane = [](float v, float limit) { return std::isfinite(v) ? llclamp(v, -limit, limit) : 0.f; };
    for (int i = 0; i < 4; ++i)
    {
        sphere.values[i] = sane(sphere.values[i], 1.0e4f);
        sphere.params[i] = sane(sphere.params[i], 4096.f);
    }
    sphere.extend_min = ((int)m_eDistExtend & (int)ESphereDistExtend::Min) != 0;
    sphere.extend_max = ((int)m_eDistExtend & (int)ESphereDistExtend::Max) != 0;
}

// static
ERlvCmdRet RlvSphereEffect::onModeChanged(const LLUUID& idRlvObj, const boost::optional<RlvBehaviourModifierValue> newValue)
{
    if (RlvSphereEffect* pEffect = LLVfxManager::instance().getEffect<RlvSphereEffect>(idRlvObj))
    {
        pEffect->m_eMode = (ESphereMode)((newValue) ? boost::get<int>(newValue.value()) : c_SphereDefaultMode);
    }
    return RLV_RET_SUCCESS;
}

// static
ERlvCmdRet RlvSphereEffect::onOriginChanged(const LLUUID& idRlvObj, const boost::optional<RlvBehaviourModifierValue> newValue)
{
    if (RlvSphereEffect* pEffect = LLVfxManager::instance().getEffect<RlvSphereEffect>(idRlvObj))
    {
        pEffect->m_eOrigin = (ESphereOrigin)((newValue) ? boost::get<int>(newValue.value()) : c_SphereDefaultOrigin);
    }
    return RLV_RET_SUCCESS;
}

// static
ERlvCmdRet RlvSphereEffect::onColorChanged(const LLUUID& idRlvObj, const boost::optional<RlvBehaviourModifierValue> newValue)
{
    if (RlvSphereEffect* pEffect = LLVfxManager::instance().getEffect<RlvSphereEffect>(idRlvObj))
    {
        LLVector4 vecColor = (newValue) ? LLVector4(boost::get<LLVector3>(newValue.value()), 1.0f) : LLVector4(c_SphereDefaultColor);
        if (!pEffect->m_nTweenDuration)
            pEffect->m_Params = vecColor;
        else
            pEffect->m_Params.start(vecColor, pEffect->m_nTweenDuration);
    }
    return RLV_RET_SUCCESS;
}

// static
ERlvCmdRet RlvSphereEffect::onDistMinChanged(const LLUUID& idRlvObj, const boost::optional<RlvBehaviourModifierValue> newValue)
{
    if (RlvSphereEffect* pEffect = LLVfxManager::instance().getEffect<RlvSphereEffect>(idRlvObj))
    {
        float nDistanceMin = (newValue) ? boost::get<float>(newValue.value()) : c_SphereDefaultDistance;
        if (!pEffect->m_nTweenDuration)
            pEffect->m_nDistanceMin = nDistanceMin;
        else
            pEffect->m_nDistanceMin.start(nDistanceMin, pEffect->m_nTweenDuration);
    }
    return RLV_RET_SUCCESS;
}

// static
ERlvCmdRet RlvSphereEffect::onDistMaxChanged(const LLUUID& idRlvObj, const boost::optional<RlvBehaviourModifierValue> newValue)
{
    if (RlvSphereEffect* pEffect = LLVfxManager::instance().getEffect<RlvSphereEffect>(idRlvObj))
    {
        float nDistanceMax = (newValue) ? boost::get<float>(newValue.value()) : c_SphereDefaultDistance;
        if (!pEffect->m_nTweenDuration)
            pEffect->m_nDistanceMax = nDistanceMax;
        else
            pEffect->m_nDistanceMax.start(nDistanceMax, pEffect->m_nTweenDuration);
    }
    return RLV_RET_SUCCESS;
}

// static
ERlvCmdRet RlvSphereEffect::onDistExtendChanged(const LLUUID& idRlvObj, const boost::optional<RlvBehaviourModifierValue> newValue)
{
    if (RlvSphereEffect* pEffect = LLVfxManager::instance().getEffect<RlvSphereEffect>(idRlvObj))
    {
        pEffect->m_eDistExtend = (ESphereDistExtend)((newValue) ? boost::get<int>(newValue.value()) : c_SphereDefaultDistanceExtend);
    }
    return RLV_RET_SUCCESS;
}

// static
ERlvCmdRet RlvSphereEffect::onParamsChanged(const LLUUID& idRlvObj, const boost::optional<RlvBehaviourModifierValue> newValue)
{
    if (RlvSphereEffect* pEffect = LLVfxManager::instance().getEffect<RlvSphereEffect>(idRlvObj))
    {
        LLVector4 params = LLVector4((newValue) ? boost::get<LLVector4>(newValue.value()).mV : c_SphereDefaultColor);
        if (!pEffect->m_nTweenDuration)
            pEffect->m_Params = params;
        else
            pEffect->m_Params.start(params, pEffect->m_nTweenDuration);
    }
    return RLV_RET_SUCCESS;
}

// static
ERlvCmdRet RlvSphereEffect::onTweenDurationChanged(const LLUUID& idRlvObj, const boost::optional<RlvBehaviourModifierValue> newValue)
{
    if (RlvSphereEffect* pEffect = LLVfxManager::instance().getEffect<RlvSphereEffect>(idRlvObj))
    {
        pEffect->m_nTweenDuration = (newValue) ? boost::get<float>(newValue.value()) : 0;
    }
    return RLV_RET_SUCCESS;
}

// static
ERlvCmdRet RlvSphereEffect::onValueMinChanged(const LLUUID& idRlvObj, const boost::optional<RlvBehaviourModifierValue> newValue)
{
    if (RlvSphereEffect* pEffect = LLVfxManager::instance().getEffect<RlvSphereEffect>(idRlvObj))
    {
        float nValueMin = (newValue) ? boost::get<float>(newValue.value()) : c_SphereDefaultAlpha;;
        if (!pEffect->m_nTweenDuration)
            pEffect->m_nValueMin = nValueMin;
        else
            pEffect->m_nValueMin.start(nValueMin, pEffect->m_nTweenDuration);
    }
    return RLV_RET_SUCCESS;
}

// static
ERlvCmdRet RlvSphereEffect::onValueMaxChanged(const LLUUID& idRlvObj, const boost::optional<RlvBehaviourModifierValue> newValue)
{
    if (RlvSphereEffect* pEffect = LLVfxManager::instance().getEffect<RlvSphereEffect>(idRlvObj))
    {
        float nValueMax = (newValue) ? boost::get<float>(newValue.value()) : c_SphereDefaultAlpha;
        if (!pEffect->m_nTweenDuration)
            pEffect->m_nValueMax = nValueMax;
        else
            pEffect->m_nValueMax.start(nValueMax, pEffect->m_nTweenDuration);
    }
    return RLV_RET_SUCCESS;
}

// ====================================================================================
