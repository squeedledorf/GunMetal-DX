/**
 * @file audioengine_openal.cpp
 * @brief implementation of audio engine using OpenAL
 * support as a OpenAL 3D implementation
 *
 * $LicenseInfo:firstyear=2002&license=viewerlgpl$
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
#include "lldir.h"
#include "llfile.h"
#include "llformat.h"

#include "llaudioengine_openal.h"
#include "lllistener_openal.h"

#include "AL/alext.h"
#include "AL/efx.h"
#ifndef AL_AUXILIARY_SEND_FILTER_GAIN_AUTO
#define AL_AUXILIARY_SEND_FILTER_GAIN_AUTO 0x2000B
#endif
#include <algorithm>
#include <cmath>
#include <vector>

// ---- #683 ACOUSTICS: EFX entry points (ALC_EXT_EFX), loaded with alGetProcAddress the first
// time acoustics is enabled (efxLoad). Null until then: nothing below calls them while off.
namespace
{
LPALGENEFFECTS               p_alGenEffects = nullptr;
LPALDELETEEFFECTS            p_alDeleteEffects = nullptr;
LPALEFFECTI                  p_alEffecti = nullptr;
LPALEFFECTF                  p_alEffectf = nullptr;
LPALEFFECTFV                 p_alEffectfv = nullptr;   // (optional: the EAX reverb pan vectors, #683 phase 1)
LPALGENFILTERS               p_alGenFilters = nullptr;
LPALDELETEFILTERS            p_alDeleteFilters = nullptr;
LPALFILTERI                  p_alFilteri = nullptr;
LPALFILTERF                  p_alFilterf = nullptr;
LPALGENAUXILIARYEFFECTSLOTS  p_alGenAuxiliaryEffectSlots = nullptr;
LPALDELETEAUXILIARYEFFECTSLOTS p_alDeleteAuxiliaryEffectSlots = nullptr;
LPALAUXILIARYEFFECTSLOTI     p_alAuxiliaryEffectSloti = nullptr;
LPALAUXILIARYEFFECTSLOTF     p_alAuxiliaryEffectSlotf = nullptr;

// ---- #892 stream speakers: OpenAL Soft extensions, loaded once (streamLoadExt). Null = absent:
// the group then starts with one alSourcePlayv and a late joiner restarts the group.
LPALSOURCEPLAYATTIMESOFT     p_alSourcePlayAtTimeSOFT = nullptr;    // AL_SOFT_source_start_delay
LPALSOURCEPLAYATTIMEVSOFT    p_alSourcePlayAtTimevSOFT = nullptr;
LPALGETSOURCEI64VSOFT        p_alGetSourcei64vSOFT = nullptr;       // AL_SOFT_source_latency
LPALCGETINTEGER64VSOFT       p_alcGetInteger64vSOFT = nullptr;      // ALC_SOFT_device_clock
bool                         s_directChannels = false;              // AL_SOFT_direct_channels

template <typename T> bool efxProc(T& fn, const char* name)
{
    fn = reinterpret_cast<T>(alGetProcAddress(name));
    return fn != nullptr;
}

// Relative change test for the reverb reload (EFX copies effect parameters into the slot
// only when the effect is re-attached, so every reload is a real update).
bool acChanged(F32 a, F32 b, F32 rel)
{
    return std::fabs(a - b) > rel * std::max(std::fabs(b), 0.01f);
}

// #683 phase 1: a pan vector moved enough to reload (absolute, the vectors are <= 1 long).
bool panChanged(const F32 a[3], const F32 b[3])
{
    const F32 dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return dx * dx + dy * dy + dz * dz > 0.06f * 0.06f;
}
} // namespace


const float LLAudioEngine_OpenAL::WIND_BUFFER_SIZE_SEC = 0.05f;

LLAudioEngine_OpenAL::LLAudioEngine_OpenAL()
    :
    mWindGen(NULL),
    mWindBuf(NULL),
    mWindBufFreq(0),
    mWindBufSamples(0),
    mWindBufBytes(0),
    mWindSource(AL_NONE),
    mNumEmptyWindALBuffers(MAX_NUM_WIND_BUFFERS)
{
}

bool LLAudioEngine_OpenAL::sEnableHRTF = false;

// virtual
LLAudioEngine_OpenAL::~LLAudioEngine_OpenAL()
{
}

// virtual
bool LLAudioEngine_OpenAL::init(void* userdata, const std::string &app_title)
{
    mWindGen = NULL;
    LLAudioEngine::init(userdata, app_title);

    // ALUT only for its decoders; the device and context are ours so we can choose the
    // attributes. alutInit(NULL, NULL) created a default context, and OpenAL Soft's
    // default `hrtf = auto` turns binaural HRTF on whenever the endpoint reports itself
    // as headphones (e.g. Realtek front-panel jacks on Windows) -- with the generic
    // dataset that sounds muffled/"like bad headphones". HRTF is now opt-in.
    if(!alutInitWithoutContext(NULL, NULL))
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::init() ALUT initialization failed: " << alutGetErrorString (alutGetError ()) << LL_ENDL;
        return false;
    }
    mDevice = alcOpenDevice(NULL);
    if (!mDevice)
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::init() alcOpenDevice failed" << LL_ENDL;
        alutExit();
        return false;
    }
    std::vector<ALCint> attrs;
    const bool has_hrtf_ext = alcIsExtensionPresent(mDevice, "ALC_SOFT_HRTF") == ALC_TRUE;
    if (has_hrtf_ext)
    {
        attrs.push_back(ALC_HRTF_SOFT);
        attrs.push_back(sEnableHRTF ? ALC_TRUE : ALC_FALSE);
    }
    if (alcIsExtensionPresent(mDevice, "ALC_EXT_EFX") == ALC_TRUE)
    {
        // Acoustics: send 0 the room reverb, 1 a reverb zone (#857), 2 the boundary bass (#897).
        // OpenAL Soft gives 2 unless asked (it allows up to 6).
        attrs.push_back(ALC_MAX_AUXILIARY_SENDS);
        attrs.push_back(4);
    }
    attrs.push_back(0);
    mContext = alcCreateContext(mDevice, attrs.data());
    if (!mContext || !alcMakeContextCurrent(mContext))
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::init() alcCreateContext failed" << LL_ENDL;
        if (mContext) alcDestroyContext(mContext);
        mContext = nullptr;
        alcCloseDevice(mDevice);
        mDevice = nullptr;
        alutExit();
        return false;
    }
    {
        ALCint freq = 0, hrtf = 0;
        alcGetIntegerv(mDevice, ALC_FREQUENCY, 1, &freq);
        if (has_hrtf_ext) alcGetIntegerv(mDevice, ALC_HRTF_SOFT, 1, &hrtf);
        LL_INFOS() << "OpenAL output: " << freq << " Hz, HRTF "
                   << (has_hrtf_ext ? (hrtf ? "on" : "off") : "unsupported")
                   << (sEnableHRTF ? " (requested on)" : " (requested off)") << LL_ENDL;
    }

    LL_INFOS() << "LLAudioEngine_OpenAL::init() OpenAL successfully initialized" << LL_ENDL;

    LL_INFOS() << "OpenAL version: "
        << ll_safe_string(alGetString(AL_VERSION)) << LL_ENDL;
    LL_INFOS() << "OpenAL vendor: "
        << ll_safe_string(alGetString(AL_VENDOR)) << LL_ENDL;
    LL_INFOS() << "OpenAL renderer: "
        << ll_safe_string(alGetString(AL_RENDERER)) << LL_ENDL;

    ALint major = alutGetMajorVersion ();
    ALint minor = alutGetMinorVersion ();
    LL_INFOS() << "ALUT version: " << major << "." << minor << LL_ENDL;

    ALCdevice *device = alcGetContextsDevice(alcGetCurrentContext());

    alcGetIntegerv(device, ALC_MAJOR_VERSION, 1, &major);
    alcGetIntegerv(device, ALC_MINOR_VERSION, 1, &minor);
    LL_INFOS() << "ALC version: " << major << "." << minor << LL_ENDL;

    LL_INFOS() << "ALC default device: "
        << ll_safe_string(alcGetString(device,
                           ALC_DEFAULT_DEVICE_SPECIFIER))
        << LL_ENDL;

    return true;
}

// virtual
std::string LLAudioEngine_OpenAL::getDriverName(bool verbose)
{
    ALCdevice *device = alcGetContextsDevice(alcGetCurrentContext());
    std::ostringstream version;

    version <<
        "OpenAL";

    if (verbose)
    {
        version <<
            ", version " <<
            ll_safe_string(alGetString(AL_VERSION)) <<
            " / " <<
            ll_safe_string(alGetString(AL_VENDOR)) <<
            " / " <<
            ll_safe_string(alGetString(AL_RENDERER));

        if (device)
            version <<
                ": " <<
                ll_safe_string(alcGetString(device,
                    ALC_DEFAULT_DEVICE_SPECIFIER));
    }

    return version.str();
}

// virtual
void LLAudioEngine_OpenAL::allocateListener()
{
    mListenerp = (LLListener *) new LLListener_OpenAL();
    if(!mListenerp)
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::allocateListener() Listener creation failed" << LL_ENDL;
    }
}

// virtual
void LLAudioEngine_OpenAL::shutdown()
{
    LL_INFOS() << "About to LLAudioEngine::shutdown()" << LL_ENDL;
    LLAudioEngine::shutdown();

    // If a subsequent error occurs while there is still an error recorded
    // internally, the second error will simply be ignored.
    // Clear previous error to make sure we will captuare a valid failure reason
    ALenum error = alutGetError();
    if (error != ALUT_ERROR_NO_ERROR)
    {
        LL_WARNS() << "Uncleared error state prior to shutdown: "
            << alutGetErrorString(error) << LL_ENDL;
    }

    // #683: the room reverb slot / effect (the channels -- and their filters -- went with
    // LLAudioEngine::shutdown above), while the context still exists.
    acousticsTeardown();
    destroyLocalLoops();   // #783 (likewise)
    streamClose();         // #892 (likewise)

    alcMakeContextCurrent(NULL);
    if (mContext) alcDestroyContext(mContext);
    mContext = nullptr;
    if (mDevice) alcCloseDevice(mDevice);
    mDevice = nullptr;

    LL_INFOS() << "About to alutExit()" << LL_ENDL;
    if(!alutExit())
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::shutdown() ALUT shutdown failed: " << alutGetErrorString (alutGetError ()) << LL_ENDL;
    }

    LL_INFOS() << "LLAudioEngine_OpenAL::shutdown() OpenAL successfully shut down" << LL_ENDL;

    delete mListenerp;
    mListenerp = NULL;
}

LLAudioBuffer *LLAudioEngine_OpenAL::createBuffer()
{
    return new LLAudioBufferOpenAL();
}

LLAudioChannel *LLAudioEngine_OpenAL::createChannel()
{
    return new LLAudioChannelOpenAL();
}

void LLAudioEngine_OpenAL::setInternalGain(F32 gain)
{
    //LL_INFOS() << "LLAudioEngine_OpenAL::setInternalGain() Gain: " << gain << LL_ENDL;
    alListenerf(AL_GAIN, gain);
}

LLAudioChannelOpenAL::LLAudioChannelOpenAL()
    :
    mALSource(AL_NONE),
    mLastSamplePos(0)
{
    alGenSources(1, &mALSource);
}

LLAudioChannelOpenAL::~LLAudioChannelOpenAL()
{
    cleanup();
    detachAcoustics(true);   // #683 (no-op unless acoustics ever attached / made a filter)
    alDeleteSources(1, &mALSource);
}

void LLAudioChannelOpenAL::cleanup()
{
    alSourceStop(mALSource);
    alSourcei(mALSource, AL_BUFFER, AL_NONE);

    mCurrentBufferp = NULL;
}

void LLAudioChannelOpenAL::play()
{
    if (mALSource == AL_NONE)
    {
        LL_WARNS() << "Playing without a mALSource, aborting" << LL_ENDL;
        return;
    }

    if(!isPlaying())
    {
        alSourcePlay(mALSource);
        // #891: only a channel with a buffer actually plays something. LL flagged the
        // source played after alSourcePlay on an empty channel too, and isDone() then
        // deleted the source before its clip had even loaded.
        if (mCurrentBufferp && getSource())
        {
            getSource()->setPlayedOnce(true);
        }
    }
}

void LLAudioChannelOpenAL::playSynced(LLAudioChannel *channelp)
{
    if (channelp)
    {
        LLAudioChannelOpenAL *masterchannelp =
            (LLAudioChannelOpenAL*)channelp;
        if (mALSource != AL_NONE &&
            masterchannelp->mALSource != AL_NONE)
        {
            // we have channels allocated to master and slave
            ALfloat master_offset;
            alGetSourcef(masterchannelp->mALSource, AL_SEC_OFFSET,
                     &master_offset);

            LL_INFOS() << "Syncing with master at " << master_offset
                << "sec" << LL_ENDL;
            // *TODO: detect when this fails, maybe use AL_SAMPLE_
            alSourcef(mALSource, AL_SEC_OFFSET, master_offset);
        }
    }
    play();
}

bool LLAudioChannelOpenAL::isPlaying()
{
    if (mALSource != AL_NONE)
    {
        ALint state;
        alGetSourcei(mALSource, AL_SOURCE_STATE, &state);
        if(state == AL_PLAYING)
        {
            return true;
        }
    }

    return false;
}

bool LLAudioChannelOpenAL::updateBuffer()
{
    if (!mCurrentSourcep)
    {
        // This channel isn't associated with any source, nothing
        // to be updated
        return false;
    }

    if (LLAudioChannel::updateBuffer())
    {
        // Base class update returned true, which means that we need to actually
        // set up the source for a different buffer.
        LLAudioBufferOpenAL *bufferp = (LLAudioBufferOpenAL *)mCurrentSourcep->getCurrentBuffer();
        ALuint buffer = bufferp->getBuffer();
        alSourcei(mALSource, AL_BUFFER, buffer);
        mLastSamplePos = 0;
    }

    if (mCurrentSourcep)
    {
        applySourceGain();
        alSourcei(mALSource, AL_LOOPING,
              mCurrentSourcep->isLoop() ? AL_TRUE : AL_FALSE);
        alSourcef(mALSource, AL_ROLLOFF_FACTOR,
              gAudiop->mListenerp->getRolloffFactor());
    }

    return true;
}


void LLAudioChannelOpenAL::updateLoop()
{
    if (mALSource == AL_NONE)
    {
        return;
    }

    // Hack:  We keep track of whether we looped or not by seeing when the
    // sample position looks like it's going backwards.  Not reliable; may
    // yield false negatives.
    //
    ALint cur_pos;
    alGetSourcei(mALSource, AL_SAMPLE_OFFSET, &cur_pos);
    if (cur_pos < mLastSamplePos)
    {
        mLoopedThisFrame = true;
    }
    mLastSamplePos = cur_pos;
}


void LLAudioChannelOpenAL::update3DPosition()
{
    if(!mCurrentSourcep)
    {
        return;
    }
    if (mCurrentSourcep->isForcedPriority())
    {
        alSource3f(mALSource, AL_POSITION, 0.0, 0.0, 0.0);
        alSource3f(mALSource, AL_VELOCITY, 0.0, 0.0, 0.0);
        alSourcei (mALSource, AL_SOURCE_RELATIVE, AL_TRUE);
    } else {
        LLVector3 float_pos;
        // #858: the acoustics driver's virtual position (diffraction pan), for this source only.
        const bool ac_pos = mAcPosOn && gAudiop && gAudiop->acousticsActive() && mAcPosSrc == mCurrentSourcep->getID()
                         && LLAudioEngine::acousticsEligible(mCurrentSourcep);
        float_pos.setVec(ac_pos ? mAcPos : mCurrentSourcep->getPositionGlobal());
        alSourcefv(mALSource, AL_POSITION, float_pos.mV);
        alSourcefv(mALSource, AL_VELOCITY, mCurrentSourcep->getVelocity().mV);
        alSourcei (mALSource, AL_SOURCE_RELATIVE, AL_FALSE);
    }

    applySourceGain();

    applyAcoustics();   // #683 (no AL call while acoustics is off and nothing is attached)
}

// #683 early gain: the level boost the acoustics driver set for THIS source, while acoustics is
// active and the source eligible; 1 otherwise.
F32 LLAudioChannelOpenAL::acousticsBoost() const
{
    if (!mCurrentSourcep || mAcBoost <= 1.f || mAcBoostSrc != mCurrentSourcep->getID() || mCurrentSourcep->isForcedPriority())
        return 1.f;
    if (!gAudiop || !gAudiop->acousticsActive() || !LLAudioEngine::acousticsEligible(mCurrentSourcep)) return 1.f;
    return llclamp(mAcBoost, 1.f, LLAudioEngine::kAcousticsMaxBoost);
}

void LLAudioChannelOpenAL::applySourceGain()
{
    if (!mCurrentSourcep || mALSource == AL_NONE) return;
    const F32 boost = acousticsBoost();
    alSourcef(mALSource, AL_GAIN, mCurrentSourcep->getGain() * getSecondaryGain() * distanceGain() * boost);
    // OpenAL clamps the attenuated gain to AL_MAX_GAIN (default 1): raised only while boosted.
    if (std::fabs(boost - mAcAppliedMaxGain) > 0.001f)
    {
        alSourcef(mALSource, AL_MAX_GAIN, boost);
        mAcAppliedMaxGain = boost;
    }
}

F32 LLAudioChannelOpenAL::distanceGain() const
{
    // (UI / preview sounds play at the listener: never.)
    if (!mCurrentSourcep || mCurrentSourcep->isForcedPriority() || !LLAudioEngine::sDistanceGain) return 1.f;
    return llclamp(LLAudioEngine::sDistanceGain(mCurrentSourcep), 0.f, 1.f);
}

// ---- #683 ACOUSTICS (EFX) ----

void LLAudioChannelOpenAL::applyAcoustics()
{
    LLAudioEngine_OpenAL* eng = static_cast<LLAudioEngine_OpenAL*>(gAudiop);
    const bool want = eng && eng->acousticsActive() && mALSource != AL_NONE
                   && LLAudioEngine::acousticsEligible(mCurrentSourcep);
    if (!want)
    {
        if (mAcAttached || mAcBassAttached) detachAcoustics(false);
        return;
    }
    // #897 boundary bass: send 2 into the EQ slot, for the source it was computed for. Its filter
    // first: the direct path's compensation only applies when the send really goes out.
    const ALuint bslot = (mAcBassDb > 0.05f && mAcBassSrc == mCurrentSourcep->getID()) ? eng->acousticsBassSlot() : 0;
    if (bslot && !mALBassFilter)
    {
        alGetError();
        p_alGenFilters(1, &mALBassFilter);
        if (alGetError() != AL_NO_ERROR || !mALBassFilter) mALBassFilter = 0;
        else
        {
            p_alFilteri(mALBassFilter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
            if (alGetError() != AL_NO_ERROR)
            {
                p_alDeleteFilters(1, &mALBassFilter);
                mALBassFilter = 0;
            }
        }
    }
    F32 bass_send = 0.f, bass_mul = 1.f;
    if (bslot && mALBassFilter) LLAudioEngine_OpenAL::bassSend(mAcBassDb, bass_send, bass_mul);
    for (ALuint* fp : { &mALFilter, &mALSendFilter })
    {
        if (*fp) continue;
        alGetError();
        p_alGenFilters(1, fp);
        if (alGetError() != AL_NO_ERROR || !*fp)
        {
            *fp = 0;
            return;
        }
        p_alFilteri(*fp, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
        if (alGetError() != AL_NO_ERROR)
        {
            p_alDeleteFilters(1, fp);
            *fp = 0;
            return;
        }
    }
    const F32 g = llclamp(mAcGain * bass_mul, 0.f, 1.f);
    const F32 hf = llclamp(mAcGainHF, 0.f, 1.f);
    if (!mAcAttached || std::fabs(g - mAcAppliedGain) > 0.003f || std::fabs(hf - mAcAppliedGainHF) > 0.003f)
    {
        // The source copies the filter's parameters when it is attached: re-attach on change.
        p_alFilterf(mALFilter, AL_LOWPASS_GAIN, g);
        p_alFilterf(mALFilter, AL_LOWPASS_GAINHF, hf);
        alSourcei(mALSource, AL_DIRECT_FILTER, (ALint)mALFilter);
        mAcAppliedGain = g;
        mAcAppliedGainHF = hf;
    }
    // Send 0 -> the room slot through its own low-pass (AudioAcousticsSendOcclusion): energy
    // reaching the room past an obstruction is the transmitted part, so it is muffled too.
    // (#683 early gain: AL_GAIN carries the boost for the dry path; the sends are divided by it
    // so the reverbs keep their level.)
    const F32 boost = acousticsBoost();
    const F32 sg = llclamp(mAcSendGain / boost, 0.f, 1.f);
    const F32 shf = llclamp(mAcSendGainHF, 0.f, 1.f);
    if (!mAcAttached || std::fabs(sg - mAcAppliedSendGain) > 0.003f || std::fabs(shf - mAcAppliedSendGainHF) > 0.003f)
    {
        p_alFilterf(mALSendFilter, AL_LOWPASS_GAIN, sg);
        p_alFilterf(mALSendFilter, AL_LOWPASS_GAINHF, shf);
        alSource3i(mALSource, AL_AUXILIARY_SEND_FILTER, (ALint)eng->acousticsSlot(), 0, (ALint)mALSendFilter);
        mAcAppliedSendGain = sg;
        mAcAppliedSendGainHF = shf;
        mAcAttached = true;
    }
    // #897: send 2 -> the boundary-bass EQ slot. Its level carries what OpenAL gives the dry path
    // from distance (a non-reverb send gets none) and the occlusion low-pass gain; HF off (the
    // shelf's floor is not needed above it).
    if (bass_send > 0.f)
    {
        const F32 bg = llclamp(bass_send * llclamp(mAcGain, 0.f, 1.f) * dryAttenuation(), 0.f, 1.f);
        if (mAcBassAttached != bslot || std::fabs(bg - mAcAppliedBass) > 0.003f)
        {
            p_alFilterf(mALBassFilter, AL_LOWPASS_GAIN, bg);
            p_alFilterf(mALBassFilter, AL_LOWPASS_GAINHF, 0.f);
            alSource3i(mALSource, AL_AUXILIARY_SEND_FILTER, (ALint)bslot, 2, (ALint)mALBassFilter);
            mAcBassAttached = bslot;
            mAcAppliedBass = bg;
        }
    }
    else if (mAcBassAttached)
        detachBassSend();
    // #857: send 1 -> this source's reverb zone (another room), through its own low-pass. Only
    // for the source the driver computed it for; the driver ramps the gain to 0 before it
    // changes the zone, so a retarget is silent.
    const ALuint zslot = (mAcZone >= 0 && mAcZoneSrc == mCurrentSourcep->getID()) ? eng->acousticsZoneSlot(mAcZone) : 0;
    const F32 zg = llclamp(mAcZoneGain / boost, 0.f, 1.f);
    if (!zslot || zg <= 0.f)
    {
        if (mAcZoneSlotAttached) detachZoneSend();
        return;
    }
    if (!mALZoneFilter)
    {
        alGetError();
        p_alGenFilters(1, &mALZoneFilter);
        if (alGetError() != AL_NO_ERROR || !mALZoneFilter)
        {
            mALZoneFilter = 0;
            return;
        }
        p_alFilteri(mALZoneFilter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
        if (alGetError() != AL_NO_ERROR)
        {
            p_alDeleteFilters(1, &mALZoneFilter);
            mALZoneFilter = 0;
            return;
        }
    }
    const F32 zhf = llclamp(mAcZoneGainHF, 0.f, 1.f);
    if (mAcZoneSlotAttached != zslot || std::fabs(zg - mAcAppliedZoneGain) > 0.003f
        || std::fabs(zhf - mAcAppliedZoneGainHF) > 0.003f)
    {
        p_alFilterf(mALZoneFilter, AL_LOWPASS_GAIN, zg);
        p_alFilterf(mALZoneFilter, AL_LOWPASS_GAINHF, zhf);
        alSource3i(mALSource, AL_AUXILIARY_SEND_FILTER, (ALint)zslot, 1, (ALint)mALZoneFilter);
        mAcZoneSlotAttached = zslot;
        mAcAppliedZoneGain = zg;
        mAcAppliedZoneGainHF = zhf;
    }
}

void LLAudioChannelOpenAL::detachZoneSend()
{
    if (mAcZoneSlotAttached && mALSource != AL_NONE)
        alSource3i(mALSource, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 1, AL_FILTER_NULL);
    mAcZoneSlotAttached = 0;
    mAcAppliedZoneGain = mAcAppliedZoneGainHF = -1.f;
}

void LLAudioChannelOpenAL::detachBassSend()
{
    if (mAcBassAttached && mALSource != AL_NONE)
        alSource3i(mALSource, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 2, AL_FILTER_NULL);
    mAcBassAttached = 0;
    mAcAppliedBass = -1.f;
}

// OpenAL's inverse distance clamped as a channel uses it (reference 1 m, the listener's rolloff,
// no maximum): the dry path's distance attenuation.
F32 LLAudioChannelOpenAL::dryAttenuation() const
{
    if (!gAudiop || !mCurrentSourcep || mCurrentSourcep->isForcedPriority()) return 1.f;
    LLVector3 pos;
    const bool ac_pos = mAcPosOn && mAcPosSrc == mCurrentSourcep->getID();
    pos.setVec(ac_pos ? mAcPos : mCurrentSourcep->getPositionGlobal());
    const F32 d = dist_vec(pos, gAudiop->getListenerPos());
    const F32 rolloff = gAudiop->mListenerp ? std::max(gAudiop->mListenerp->getRolloffFactor(), 0.f) : 1.f;
    return 1.f / (1.f + rolloff * (std::max(d, 1.f) - 1.f));
}

void LLAudioChannelOpenAL::detachAcoustics(bool delete_filter)
{
    if (mAcAttached && mALSource != AL_NONE)
    {
        alSourcei(mALSource, AL_DIRECT_FILTER, AL_FILTER_NULL);
        alSource3i(mALSource, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
    }
    detachZoneSend();   // #857
    detachBassSend();   // #897
    mAcAttached = false;
    mAcAppliedGain = mAcAppliedGainHF = -1.f;
    mAcAppliedSendGain = mAcAppliedSendGainHF = -1.f;
    if (delete_filter && p_alDeleteFilters)
    {
        if (mALFilter) { p_alDeleteFilters(1, &mALFilter); mALFilter = 0; }
        if (mALSendFilter) { p_alDeleteFilters(1, &mALSendFilter); mALSendFilter = 0; }
        if (mALZoneFilter) { p_alDeleteFilters(1, &mALZoneFilter); mALZoneFilter = 0; }
        if (mALBassFilter) { p_alDeleteFilters(1, &mALBassFilter); mALBassFilter = 0; }
    }
}

bool LLAudioEngine_OpenAL::efxLoad()
{
    if (!mDevice || alcIsExtensionPresent(mDevice, "ALC_EXT_EFX") != ALC_TRUE)
    {
        LL_WARNS("Acoustics") << "OpenAL device has no ALC_EXT_EFX: acoustics unavailable" << LL_ENDL;
        return false;
    }
    ALCint sends = 0;
    alcGetIntegerv(mDevice, ALC_MAX_AUXILIARY_SENDS, 1, &sends);
    if (sends < 1)
    {
        LL_WARNS("Acoustics") << "OpenAL context has no auxiliary sends: acoustics unavailable" << LL_ENDL;
        return false;
    }
    const bool ok = efxProc(p_alGenEffects, "alGenEffects") && efxProc(p_alDeleteEffects, "alDeleteEffects")
                 && efxProc(p_alEffecti, "alEffecti") && efxProc(p_alEffectf, "alEffectf")
                 && efxProc(p_alGenFilters, "alGenFilters") && efxProc(p_alDeleteFilters, "alDeleteFilters")
                 && efxProc(p_alFilteri, "alFilteri") && efxProc(p_alFilterf, "alFilterf")
                 && efxProc(p_alGenAuxiliaryEffectSlots, "alGenAuxiliaryEffectSlots")
                 && efxProc(p_alDeleteAuxiliaryEffectSlots, "alDeleteAuxiliaryEffectSlots")
                 && efxProc(p_alAuxiliaryEffectSloti, "alAuxiliaryEffectSloti")
                 && efxProc(p_alAuxiliaryEffectSlotf, "alAuxiliaryEffectSlotf");
    if (!ok)
    {
        LL_WARNS("Acoustics") << "OpenAL EFX entry points missing: acoustics unavailable" << LL_ENDL;
        return false;
    }
    if (!efxProc(p_alEffectfv, "alEffectfv")) p_alEffectfv = nullptr;   // (pans then stay centred)
    mAcSends = sends;
    LL_INFOS("Acoustics") << "OpenAL EFX available (" << sends << " auxiliary sends per source; reverb zones "
                          << (sends >= 2 ? "available" : "unavailable: need 2") << ", boundary bass "
                          << (sends >= 3 ? "available" : "unavailable: need 3") << ")" << LL_ENDL;
    return true;
}

bool LLAudioEngine_OpenAL::setAcousticsEnabled(bool on)
{
    if (!on)
    {
        if (mAcOn)
        {
            acousticsTeardown();
            LL_INFOS("Acoustics") << "EFX acoustics off (filters / sends detached, slot deleted)" << LL_ENDL;
        }
        return false;
    }
    if (mAcOn) return true;
    if (!mContext || !mDevice) return false;
    if (!mEfxTried)
    {
        mEfxTried = true;
        mEfxOk = efxLoad();
    }
    if (!mEfxOk) return false;

    alGetError();
    p_alGenAuxiliaryEffectSlots(1, &mAcSlot);
    if (alGetError() != AL_NO_ERROR) mAcSlot = 0;
    p_alGenEffects(1, &mAcEffect);
    if (alGetError() != AL_NO_ERROR) mAcEffect = 0;
    bool ok = mAcSlot && mAcEffect;
    if (ok)
    {
        p_alEffecti(mAcEffect, AL_EFFECT_TYPE, AL_EFFECT_EAXREVERB);
        mAcEax = alGetError() == AL_NO_ERROR;
        if (!mAcEax)
        {
            p_alEffecti(mAcEffect, AL_EFFECT_TYPE, AL_EFFECT_REVERB);
            ok = alGetError() == AL_NO_ERROR;
        }
    }
    if (!ok)
    {
        LL_WARNS("Acoustics") << "EFX reverb slot / effect creation failed: acoustics stays off" << LL_ENDL;
        acousticsTeardown();
        return false;
    }
    mAcOn = true;
    // Start dry (slot gain 0) with the generic preset loaded; the driver fades it in.
    mAcAppliedValid = false;
    mAcWetApplied = -1.f;
    LLAcousticsReverb dry;
    setAcousticsReverb(dry);
    LL_INFOS("Acoustics") << "EFX acoustics on (" << (mAcEax ? "EAX reverb" : "standard reverb")
                          << " slot, low-pass direct filter per in-world source)" << LL_ENDL;
    return true;
}

void LLAudioEngine_OpenAL::acousticsTeardown()
{
    for (S32 i = 0; i < LL_MAX_AUDIO_CHANNELS; ++i)
    {
        if (mChannels[i]) static_cast<LLAudioChannelOpenAL*>(mChannels[i])->detachAcoustics(true);
    }
    detachShotSends();   // #783 thunder: nothing may point at the slot being deleted
    detachStreamSends(0);   // #892 speakers (room send and zone sends)
    deleteZones(0);      // #857 (the channels' send 1 is already detached above)
    deleteBass();        // #897 (the channels' / speakers' send 2 already detached above)
    if (mAcSlot && p_alAuxiliaryEffectSloti)
    {
        p_alAuxiliaryEffectSloti(mAcSlot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
        p_alDeleteAuxiliaryEffectSlots(1, &mAcSlot);
    }
    if (mAcEffect && p_alDeleteEffects) p_alDeleteEffects(1, &mAcEffect);
    const bool had = mAcOn || mAcSlot || mAcEffect;
    mAcSlot = 0;
    mAcEffect = 0;
    mAcOn = false;
    mAcAppliedValid = false;
    mAcWetApplied = -1.f;
    if (had) alGetError();   // (leave no EFX error behind for the next unrelated check)
}

void LLAudioEngine_OpenAL::writeReverb(ALuint effect, const LLAcousticsReverb& r)
{
    if (mAcEax)
    {
        p_alEffectf(effect, AL_EAXREVERB_DENSITY, llclamp(r.density, 0.f, 1.f));
        p_alEffectf(effect, AL_EAXREVERB_DIFFUSION, llclamp(r.diffusion, 0.f, 1.f));
        p_alEffectf(effect, AL_EAXREVERB_GAIN, llclamp(r.gain, 0.f, 1.f));
        p_alEffectf(effect, AL_EAXREVERB_GAINHF, llclamp(r.gain_hf, 0.f, 1.f));
        p_alEffectf(effect, AL_EAXREVERB_DECAY_TIME, llclamp(r.decay_time, 0.1f, 20.f));
        p_alEffectf(effect, AL_EAXREVERB_DECAY_HFRATIO, llclamp(r.decay_hf_ratio, 0.1f, 2.f));
        p_alEffectf(effect, AL_EAXREVERB_REFLECTIONS_GAIN, llclamp(r.reflections_gain, 0.f, 3.16f));
        p_alEffectf(effect, AL_EAXREVERB_REFLECTIONS_DELAY, llclamp(r.reflections_delay, 0.f, 0.3f));
        p_alEffectf(effect, AL_EAXREVERB_LATE_REVERB_GAIN, llclamp(r.late_gain, 0.f, 10.f));
        p_alEffectf(effect, AL_EAXREVERB_LATE_REVERB_DELAY, llclamp(r.late_delay, 0.f, 0.1f));
        // #683 phase 1: the measured LF decay / level and the early / late pans. The bands are
        // measured at EAX's own references (LF 250 Hz, HF 5 kHz), set explicitly here.
        p_alEffectf(effect, AL_EAXREVERB_GAINLF, llclamp(r.gain_lf, 0.f, 1.f));
        p_alEffectf(effect, AL_EAXREVERB_DECAY_LFRATIO, llclamp(r.decay_lf_ratio, 0.1f, 2.f));
        p_alEffectf(effect, AL_EAXREVERB_LFREFERENCE, 250.f);
        p_alEffectf(effect, AL_EAXREVERB_HFREFERENCE, 5000.f);
        if (p_alEffectfv)
        {
            ALfloat rp[3], lp[3];
            for (U32 i = 0; i < 3; ++i)
            {
                rp[i] = llclamp(r.reflections_pan[i], -1.f, 1.f);
                lp[i] = llclamp(r.late_pan[i], -1.f, 1.f);
            }
            p_alEffectfv(effect, AL_EAXREVERB_REFLECTIONS_PAN, rp);
            p_alEffectfv(effect, AL_EAXREVERB_LATE_REVERB_PAN, lp);
        }
    }
    else
    {
        p_alEffectf(effect, AL_REVERB_DENSITY, llclamp(r.density, 0.f, 1.f));
        p_alEffectf(effect, AL_REVERB_DIFFUSION, llclamp(r.diffusion, 0.f, 1.f));
        p_alEffectf(effect, AL_REVERB_GAIN, llclamp(r.gain, 0.f, 1.f));
        p_alEffectf(effect, AL_REVERB_GAINHF, llclamp(r.gain_hf, 0.f, 1.f));
        p_alEffectf(effect, AL_REVERB_DECAY_TIME, llclamp(r.decay_time, 0.1f, 20.f));
        p_alEffectf(effect, AL_REVERB_DECAY_HFRATIO, llclamp(r.decay_hf_ratio, 0.1f, 2.f));
        p_alEffectf(effect, AL_REVERB_REFLECTIONS_GAIN, llclamp(r.reflections_gain, 0.f, 3.16f));
        p_alEffectf(effect, AL_REVERB_REFLECTIONS_DELAY, llclamp(r.reflections_delay, 0.f, 0.3f));
        p_alEffectf(effect, AL_REVERB_LATE_REVERB_GAIN, llclamp(r.late_gain, 0.f, 10.f));
        p_alEffectf(effect, AL_REVERB_LATE_REVERB_DELAY, llclamp(r.late_delay, 0.f, 0.1f));
    }
}

void LLAudioEngine_OpenAL::setAcousticsReverb(const LLAcousticsReverb& r)
{
    if (!mAcOn || !mAcSlot || !mAcEffect) return;
    applyReverb(mAcSlot, mAcEffect, r, mAcApplied, mAcAppliedValid, mAcWetApplied);
}

void LLAudioEngine_OpenAL::applyReverb(ALuint slot, ALuint effect, const LLAcousticsReverb& r, LLAcousticsReverb& applied,
                                       bool& applied_valid, F32& wet_applied)
{
    const F32 wet = llclamp(r.wet, 0.f, 1.f);
    if (std::fabs(wet - wet_applied) > 0.002f)
    {
        p_alAuxiliaryEffectSlotf(slot, AL_EFFECTSLOT_GAIN, wet);
        wet_applied = wet;
    }
    const LLAcousticsReverb& a = applied;
    const bool changed = !applied_valid
        || acChanged(r.decay_time, a.decay_time, 0.03f) || acChanged(r.decay_hf_ratio, a.decay_hf_ratio, 0.03f)
        || acChanged(r.density, a.density, 0.03f) || acChanged(r.diffusion, a.diffusion, 0.03f)
        || acChanged(r.gain, a.gain, 0.03f) || acChanged(r.gain_hf, a.gain_hf, 0.03f)
        || acChanged(r.reflections_gain, a.reflections_gain, 0.05f)
        || acChanged(r.reflections_delay, a.reflections_delay, 0.05f)
        || acChanged(r.late_gain, a.late_gain, 0.05f) || acChanged(r.late_delay, a.late_delay, 0.05f)
        // (#683 phase 1 fields: only where writeReverb writes them -- EAX, and the pans with alEffectfv.)
        || (mAcEax && (acChanged(r.decay_lf_ratio, a.decay_lf_ratio, 0.03f) || acChanged(r.gain_lf, a.gain_lf, 0.03f)))
        || (mAcEax && p_alEffectfv
            && (panChanged(r.reflections_pan, a.reflections_pan) || panChanged(r.late_pan, a.late_pan)));
    if (!changed) return;
    writeReverb(effect, r);
    // The slot copies the effect's parameters when it is (re)attached.
    p_alAuxiliaryEffectSloti(slot, AL_EFFECTSLOT_EFFECT, (ALint)effect);
    applied = r;
    applied_valid = true;
}

// ---- #897 BOUNDARY BASS ----
// The equalizer: low shelf at kAcousticsBassShelfHz up kBassEqGain (+12 dB, the effect allows
// +18), mid 1 / mid 2 / high at the effect's floor (-18 dB). Sent in parallel to the dry path:
// low end = dry + send x kBassEqGain, above the shelf = dry + send x kBassEqFloor -- so the
// direct path is lowered by send x kBassEqFloor to keep the mids where they were.
namespace
{
constexpr F32 kBassEqGain = 3.981f;    // +12 dB
constexpr F32 kBassEqFloor = 0.126f;   // AL_EQUALIZER_MIN_*_GAIN (-18 dB)
}

// static
void LLAudioEngine_OpenAL::bassSend(F32 db, F32& send, F32& direct_mul)
{
    const F32 t = std::pow(10.f, llclamp(db, 0.f, kAcousticsBassMaxDb) / 20.f);   // target low-end gain
    send = llclamp((t - 1.f) / (kBassEqGain - kBassEqFloor), 0.f, 1.f);
    direct_mul = 1.f - kBassEqFloor * send;
}

void LLAudioEngine_OpenAL::deleteBass()
{
    if (mAcBassSlot)
    {
        for (S32 i = 0; i < LL_MAX_AUDIO_CHANNELS; ++i)
        {
            LLAudioChannelOpenAL* ch = static_cast<LLAudioChannelOpenAL*>(mChannels[i]);
            if (ch && ch->mAcBassAttached) ch->detachBassSend();
        }
        detachStreamSends(mAcBassSlot);
        if (p_alAuxiliaryEffectSloti)
        {
            p_alAuxiliaryEffectSloti(mAcBassSlot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
            p_alDeleteAuxiliaryEffectSlots(1, &mAcBassSlot);
        }
    }
    if (mAcBassEffect && p_alDeleteEffects) p_alDeleteEffects(1, &mAcBassEffect);
    const bool had = mAcBassSlot || mAcBassEffect;
    mAcBassSlot = 0;
    mAcBassEffect = 0;
    if (had) alGetError();
}

bool LLAudioEngine_OpenAL::setAcousticsBass(bool on)
{
    if (!on || !mAcOn)
    {
        if (mAcBassSlot || mAcBassEffect)
        {
            deleteBass();
            LL_INFOS("Acoustics") << "EFX boundary bass off" << LL_ENDL;
        }
        return false;
    }
    if (mAcBassSlot) return true;
    if (mAcSends < 3)
    {
        if (!mAcBassWarned)
        {
            mAcBassWarned = true;
            LL_INFOS("Acoustics") << "EFX boundary bass unavailable: the context has " << mAcSends
                                  << " auxiliary sends per source (needs 3)" << LL_ENDL;
        }
        return false;
    }
    alGetError();
    p_alGenAuxiliaryEffectSlots(1, &mAcBassSlot);
    if (alGetError() != AL_NO_ERROR) mAcBassSlot = 0;
    p_alGenEffects(1, &mAcBassEffect);
    if (alGetError() != AL_NO_ERROR) mAcBassEffect = 0;
    bool ok = mAcBassSlot && mAcBassEffect;
    if (ok)
    {
        p_alEffecti(mAcBassEffect, AL_EFFECT_TYPE, AL_EFFECT_EQUALIZER);
        ok = alGetError() == AL_NO_ERROR;
    }
    if (ok)
    {
        p_alEffectf(mAcBassEffect, AL_EQUALIZER_LOW_GAIN, kBassEqGain);
        p_alEffectf(mAcBassEffect, AL_EQUALIZER_LOW_CUTOFF, kAcousticsBassShelfHz);
        p_alEffectf(mAcBassEffect, AL_EQUALIZER_MID1_GAIN, kBassEqFloor);
        p_alEffectf(mAcBassEffect, AL_EQUALIZER_MID2_GAIN, kBassEqFloor);
        p_alEffectf(mAcBassEffect, AL_EQUALIZER_HIGH_GAIN, kBassEqFloor);
        p_alAuxiliaryEffectSloti(mAcBassSlot, AL_EFFECTSLOT_EFFECT, (ALint)mAcBassEffect);
        p_alAuxiliaryEffectSlotf(mAcBassSlot, AL_EFFECTSLOT_GAIN, 1.f);
        ok = alGetError() == AL_NO_ERROR;
    }
    if (!ok)
    {
        deleteBass();
        LL_WARNS("Acoustics") << "EFX boundary bass: equalizer slot / effect creation failed" << LL_ENDL;
        return false;
    }
    LL_INFOS("Acoustics") << "EFX boundary bass on (equalizer slot on send 2: low shelf " << kAcousticsBassShelfHz
                          << " Hz)" << LL_ENDL;
    return true;
}

// ---- #857 REVERB ZONES ----

U32 LLAudioEngine_OpenAL::acousticsZoneCapacity() const
{
    return (mAcOn && mAcSends >= 2) ? kAcousticsMaxZones : 0;
}

void LLAudioEngine_OpenAL::deleteZones(U32 from)
{
    if (from >= mAcZoneCount) return;
    // Nothing may point at a slot being deleted: detach the channels' send 1 into these zones.
    for (S32 i = 0; i < LL_MAX_AUDIO_CHANNELS; ++i)
    {
        LLAudioChannelOpenAL* ch = static_cast<LLAudioChannelOpenAL*>(mChannels[i]);
        if (!ch || !ch->mAcZoneSlotAttached) continue;
        for (U32 z = from; z < mAcZoneCount; ++z)
            if (ch->mAcZoneSlotAttached == mAcZones[z].slot) ch->detachZoneSend();
    }
    for (U32 z = from; z < mAcZoneCount; ++z)
        if (mAcZones[z].slot) detachStreamSends(mAcZones[z].slot);   // #892 speakers' send 1
    for (U32 z = from; z < mAcZoneCount; ++z)
    {
        AcZone& zn = mAcZones[z];
        if (zn.slot && p_alAuxiliaryEffectSloti)
        {
            p_alAuxiliaryEffectSloti(zn.slot, AL_EFFECTSLOT_EFFECT, AL_EFFECT_NULL);
            p_alDeleteAuxiliaryEffectSlots(1, &zn.slot);
        }
        if (zn.effect && p_alDeleteEffects) p_alDeleteEffects(1, &zn.effect);
        zn = AcZone{};
    }
    mAcZoneCount = from;
    alGetError();
}

U32 LLAudioEngine_OpenAL::setAcousticsZoneCount(U32 count)
{
    count = std::min(count, acousticsZoneCapacity());
    if (count < mAcZoneCount)
    {
        deleteZones(count);
        LL_INFOS("Acoustics") << "EFX reverb zones: " << mAcZoneCount << LL_ENDL;
        return mAcZoneCount;
    }
    const U32 before = mAcZoneCount;
    while (mAcZoneCount < count)
    {
        // (There is no query for the slot limit: generate and check.)
        AcZone& zn = mAcZones[mAcZoneCount];
        alGetError();
        p_alGenAuxiliaryEffectSlots(1, &zn.slot);
        if (alGetError() != AL_NO_ERROR) zn.slot = 0;
        p_alGenEffects(1, &zn.effect);
        if (alGetError() != AL_NO_ERROR) zn.effect = 0;
        bool ok = zn.slot && zn.effect;
        if (ok)
        {
            p_alEffecti(zn.effect, AL_EFFECT_TYPE, mAcEax ? AL_EFFECT_EAXREVERB : AL_EFFECT_REVERB);
            ok = alGetError() == AL_NO_ERROR;
        }
        if (ok)
        {
            // Silent until the driver configures it (slot gain 0).
            p_alAuxiliaryEffectSlotf(zn.slot, AL_EFFECTSLOT_GAIN, 0.f);
            zn.wet_applied = 0.f;
            ok = alGetError() == AL_NO_ERROR;
        }
        if (!ok)
        {
            if (zn.slot) p_alDeleteAuxiliaryEffectSlots(1, &zn.slot);
            if (zn.effect) p_alDeleteEffects(1, &zn.effect);
            zn = AcZone{};
            alGetError();
            LL_WARNS("Acoustics") << "EFX reverb zone " << mAcZoneCount << " could not be created (slot limit?)" << LL_ENDL;
            break;
        }
        ++mAcZoneCount;
    }
    if (mAcZoneCount != before) LL_INFOS("Acoustics") << "EFX reverb zones: " << mAcZoneCount << LL_ENDL;
    return mAcZoneCount;
}

void LLAudioEngine_OpenAL::setAcousticsZoneReverb(U32 zone, const LLAcousticsReverb& r)
{
    if (!mAcOn || zone >= mAcZoneCount) return;
    AcZone& zn = mAcZones[zone];
    if (!zn.slot || !zn.effect) return;
    applyReverb(zn.slot, zn.effect, r, zn.applied, zn.applied_valid, zn.wet_applied);
}

LLAudioBufferOpenAL::LLAudioBufferOpenAL()
{
    mALBuffer = AL_NONE;
}

LLAudioBufferOpenAL::~LLAudioBufferOpenAL()
{
    cleanup();
}

void LLAudioBufferOpenAL::cleanup()
{
    if(mALBuffer != AL_NONE)
    {
        alGetError(); // clear error
        alDeleteBuffers(1, &mALBuffer);

        ALenum error = alutGetError();
        if(ALC_NO_ERROR != error)
        {
            LL_WARNS("OpenAL") << "Error: " << alutGetErrorString( error ) << " when cleaning up a buffer" << LL_ENDL;
        }
        mALBuffer = AL_NONE;
    }
}

bool LLAudioBufferOpenAL::loadWAV(const std::string& filename)
{
    cleanup();
    // Read through LLFile, not alutCreateBufferFromFile: ALUT opens the path with a
    // narrow fopen(), which on Windows interprets it in the ANSI codepage. Our paths
    // are UTF-8, so a user profile with non-ASCII characters (e.g. C:\Users\João)
    // made every decoded sound fail with "I/O error" -- only the synthesised wind,
    // which needs no file, still played (#146). LLFile opens via the wide-char API.
    const std::string data = LLFile::getContents(filename);
    if (!data.empty())
    {
        mALBuffer = alutCreateBufferFromFileImage(data.data(), (ALsizei)data.size());
    }
    if(mALBuffer == AL_NONE)
    {
        ALenum error = alutGetError();
        if (LLFile::isfile(filename))
        {
            LL_WARNS() <<
                "LLAudioBufferOpenAL::loadWAV() Error loading "
                << filename
                << " " << alutGetErrorString(error) << LL_ENDL;
        }
        else
        {
            // It's common for the file to not actually exist.
            LL_DEBUGS() <<
                "LLAudioBufferOpenAL::loadWAV() Error loading "
                 << filename
                 << " " << alutGetErrorString(error) << LL_ENDL;
        }
        return false;
    }

    return true;
}

U32 LLAudioBufferOpenAL::getLength()
{
    if(mALBuffer == AL_NONE)
    {
        return 0;
    }
    ALint length;
    alGetBufferi(mALBuffer, AL_SIZE, &length);
    return length / 2; // convert size in bytes to size in (16-bit) samples
}

// ------------

// ---- LOCAL LOOPS (#783 weather ambience; llaudioengine.h) ----
U32 LLAudioEngine_OpenAL::createLocalLoop(const S16* pcm, U32 frames, U32 channels, U32 rate)
{
    return createLocalSource(pcm, frames, channels, rate, true);
}

U32 LLAudioEngine_OpenAL::createLocalSource(const S16* pcm, U32 frames, U32 channels, U32 rate, bool loop)
{
    if (!mContext || !pcm || !frames || (channels != 1 && channels != 2) || !rate) return 0;
    alGetError();   // (clear)
    LocalLoop l;
    alGenBuffers(1, &l.buffer);
    if (alGetError() != AL_NO_ERROR) return 0;
    alBufferData(l.buffer, channels == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16, pcm,
                 (ALsizei)(frames * channels * sizeof(S16)), (ALsizei)rate);
    if (alGetError() != AL_NO_ERROR)
    {
        alDeleteBuffers(1, &l.buffer);
        return 0;
    }
    alGenSources(1, &l.source);
    if (alGetError() != AL_NO_ERROR)
    {
        alDeleteBuffers(1, &l.buffer);
        return 0;
    }
    alSourcei(l.source, AL_BUFFER, (ALint)l.buffer);
    alSourcei(l.source, AL_LOOPING, loop ? AL_TRUE : AL_FALSE);
    // Loops: at the listener (no panning, no distance). Shots: placed in the world each set
    // call (listener + offset), still no distance attenuation.
    alSourcei(l.source, AL_SOURCE_RELATIVE, loop ? AL_TRUE : AL_FALSE);
    alSource3f(l.source, AL_POSITION, 0.f, 0.f, 0.f);
    alSource3f(l.source, AL_VELOCITY, 0.f, 0.f, 0.f);
    alSourcef(l.source, AL_ROLLOFF_FACTOR, 0.f);
    alSourcef(l.source, AL_GAIN, 0.f);
    l.shot = !loop;
    l.frames = frames;
    const U32 id = mNextLocalLoop++;
    mLocalLoops[id] = l;
    return id;
}

// #1012: another looping source over the same AL buffer (the buffer stays owned by `base`).
U32 LLAudioEngine_OpenAL::createLocalLoopShared(U32 base)
{
    auto it = mLocalLoops.find(base);
    if (!mContext || it == mLocalLoops.end() || it->second.shot || !it->second.ownsBuffer) return 0;
    alGetError();   // (clear)
    LocalLoop l;
    l.buffer = it->second.buffer;
    l.frames = it->second.frames;
    l.ownsBuffer = false;
    alGenSources(1, &l.source);
    if (alGetError() != AL_NO_ERROR) return 0;
    alSourcei(l.source, AL_BUFFER, (ALint)l.buffer);
    alSourcei(l.source, AL_LOOPING, AL_TRUE);
    alSourcei(l.source, AL_SOURCE_RELATIVE, AL_TRUE);
    alSource3f(l.source, AL_POSITION, 0.f, 0.f, 0.f);
    alSource3f(l.source, AL_VELOCITY, 0.f, 0.f, 0.f);
    alSourcef(l.source, AL_ROLLOFF_FACTOR, 0.f);
    alSourcef(l.source, AL_GAIN, 0.f);
    const U32 id = mNextLocalLoop++;
    mLocalLoops[id] = l;
    return id;
}

void LLAudioEngine_OpenAL::seekLocalLoop(U32 id, U32 frame)
{
    auto it = mLocalLoops.find(id);
    if (it == mLocalLoops.end() || it->second.shot || !it->second.frames) return;
    alSourcei(it->second.source, AL_SAMPLE_OFFSET, (ALint)(frame % it->second.frames));
}

S32 LLAudioEngine_OpenAL::localLoopPosition(U32 id)
{
    auto it = mLocalLoops.find(id);
    if (it == mLocalLoops.end() || it->second.shot) return -1;
    ALint pos = 0;
    alGetError();
    alGetSourcei(it->second.source, AL_SAMPLE_OFFSET, &pos);
    return alGetError() == AL_NO_ERROR ? (S32)pos : -1;
}

// ---- LOCAL SHOTS (#783 thunder; llaudioengine.h) ----
bool LLAudioEngine_OpenAL::efxReady()
{
    if (!mContext || !mDevice) return false;
    if (!mEfxTried)
    {
        mEfxTried = true;
        mEfxOk = efxLoad();
    }
    return mEfxOk;
}

U32 LLAudioEngine_OpenAL::createLocalShot(const S16* pcm, U32 frames, U32 channels, U32 rate)
{
    return createLocalSource(pcm, frames, channels, rate, false);
}

void LLAudioEngine_OpenAL::playLocalShot(U32 id, const LLVector3& offset, F32 gain, F32 pitch)
{
    auto it = mLocalLoops.find(id);
    if (it == mLocalLoops.end() || !it->second.shot) return;
    LocalLoop& l = it->second;
    alSourceStop(l.source);
    alSourceRewind(l.source);
    l.pitch = llclamp(pitch, 0.5f, 2.f);
    alSourcef(l.source, AL_PITCH, l.pitch);
    // (The filters keep what the last setLocalShot applied: call it first for a new shot.)
    const LLVector3 pos = getListenerPos() + offset;
    alSourcefv(l.source, AL_POSITION, pos.mV);
    if (l.gain < 0.f) l.gain = llclamp(gain, 0.f, 1.f);
    alSourcef(l.source, AL_GAIN, l.gain);
    alSourcePlay(l.source);
    l.playing = true;
}

void LLAudioEngine_OpenAL::setLocalShot(U32 id, const LLVector3& offset, F32 gain, F32 direct_gain, F32 direct_hf, F32 send)
{
    auto it = mLocalLoops.find(id);
    if (it == mLocalLoops.end() || !it->second.shot) return;
    LocalLoop& l = it->second;
    const LLVector3 pos = getListenerPos() + offset;
    alSourcefv(l.source, AL_POSITION, pos.mV);
    direct_gain = llclamp(direct_gain, 0.f, 1.f);
    direct_hf = llclamp(direct_hf, 0.f, 1.f);
    send = llclamp(send, 0.f, 1.f);
    const bool efx = efxReady();
    // Without EFX the low-pass cannot exist: fold a part of the HF cut into the level.
    const F32 g = llclamp(gain * (efx ? 1.f : direct_gain * (0.5f + 0.5f * direct_hf)), 0.f, 1.f);
    if (std::fabs(g - l.gain) > 1e-4f)
    {
        alSourcef(l.source, AL_GAIN, g);
        l.gain = g;
    }
    if (!efx) return;
    alGetError();
    if (!l.filter)
    {
        p_alGenFilters(1, &l.filter);
        if (alGetError() != AL_NO_ERROR) l.filter = 0;
        else p_alFilteri(l.filter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
    }
    if (l.filter && (std::fabs(direct_gain - l.appliedDirect) > 0.003f || std::fabs(direct_hf - l.appliedHF) > 0.003f))
    {
        // (The source copies the filter's parameters when it is attached: re-attach on change.)
        p_alFilterf(l.filter, AL_LOWPASS_GAIN, direct_gain);
        p_alFilterf(l.filter, AL_LOWPASS_GAINHF, direct_hf);
        alSourcei(l.source, AL_DIRECT_FILTER, (ALint)l.filter);
        l.appliedDirect = direct_gain;
        l.appliedHF = direct_hf;
    }
    // The room send: only while the acoustics reverb slot exists.
    if (mAcOn && mAcSlot)
    {
        if (!l.sendFilter)
        {
            p_alGenFilters(1, &l.sendFilter);
            if (alGetError() != AL_NO_ERROR) l.sendFilter = 0;
            else p_alFilteri(l.sendFilter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
        }
        if (l.sendFilter && (!l.sendAttached || std::fabs(send - l.appliedSend) > 0.003f))
        {
            p_alFilterf(l.sendFilter, AL_LOWPASS_GAIN, send);
            p_alFilterf(l.sendFilter, AL_LOWPASS_GAINHF, 1.f);
            alSource3i(l.source, AL_AUXILIARY_SEND_FILTER, (ALint)mAcSlot, 0, (ALint)l.sendFilter);
            l.appliedSend = send;
            l.sendAttached = true;
        }
    }
    else if (l.sendAttached)
    {
        alSource3i(l.source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
        l.sendAttached = false;
        l.appliedSend = -1.f;
    }
    alGetError();   // (leave no EFX error behind)
}

bool LLAudioEngine_OpenAL::localShotPlaying(U32 id)
{
    auto it = mLocalLoops.find(id);
    if (it == mLocalLoops.end() || !it->second.shot || !it->second.playing) return false;
    ALint state = AL_STOPPED;
    alGetSourcei(it->second.source, AL_SOURCE_STATE, &state);
    it->second.playing = state == AL_PLAYING || state == AL_INITIAL;
    return it->second.playing;
}

void LLAudioEngine_OpenAL::detachShotSends()
{
    for (auto& kv : mLocalLoops)
    {
        LocalLoop& l = kv.second;
        if (!l.sendAttached) continue;
        alSource3i(l.source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
        l.sendAttached = false;
        l.appliedSend = -1.f;
    }
}

void LLAudioEngine_OpenAL::setLocalLoop(U32 id, F32 gain, F32 pitch)
{
    auto it = mLocalLoops.find(id);
    if (it == mLocalLoops.end()) return;
    LocalLoop& l = it->second;
    gain = llclamp(gain, 0.f, 1.f);
    pitch = llclamp(pitch, 0.5f, 2.f);
    if (std::fabs(gain - l.gain) > 1e-4f)
    {
        alSourcef(l.source, AL_GAIN, gain);
        l.gain = gain;
    }
    if (std::fabs(pitch - l.pitch) > 1e-4f)
    {
        alSourcef(l.source, AL_PITCH, pitch);
        l.pitch = pitch;
    }
    if (gain > 0.f && !l.playing)
    {
        alSourcePlay(l.source);
        l.playing = true;
    }
    else if (gain <= 0.f && l.playing)
    {
        alSourcePause(l.source);   // (resumes where it was: no restart click)
        l.playing = false;
    }
}

// #894: a loop placed relative to the listener (world axes), or back at the listener.
void LLAudioEngine_OpenAL::setLocalLoopOffset(U32 id, const LLVector3& offset)
{
    auto it = mLocalLoops.find(id);
    if (it == mLocalLoops.end() || it->second.shot) return;
    LocalLoop& l = it->second;
    const bool placed = offset.lengthSquared() > 1e-6f;
    if (placed != l.placed)
    {
        alSourcei(l.source, AL_SOURCE_RELATIVE, placed ? AL_FALSE : AL_TRUE);
        l.placed = placed;
    }
    if (placed)
    {
        const LLVector3 pos = getListenerPos() + offset;
        alSourcefv(l.source, AL_POSITION, pos.mV);
    }
    else
        alSource3f(l.source, AL_POSITION, 0.f, 0.f, 0.f);
}

void LLAudioEngine_OpenAL::destroyLocalLoop(U32 id)
{
    auto it = mLocalLoops.find(id);
    if (it == mLocalLoops.end()) return;
    if (it->second.ownsBuffer && !it->second.shot)
    {
        // #1012: sources sharing this buffer go first (they would play a deleted buffer).
        const ALuint buf = it->second.buffer;
        for (auto sh = mLocalLoops.begin(); sh != mLocalLoops.end();)
        {
            if (!sh->second.ownsBuffer && sh->second.buffer == buf)
            {
                alSourceStop(sh->second.source);
                alDeleteSources(1, &sh->second.source);
                sh = mLocalLoops.erase(sh);
            }
            else ++sh;
        }
        it = mLocalLoops.find(id);
    }
    alSourceStop(it->second.source);
    if (it->second.filter || it->second.sendFilter)
    {
        alSourcei(it->second.source, AL_DIRECT_FILTER, AL_FILTER_NULL);
        alSource3i(it->second.source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
        if (p_alDeleteFilters)
        {
            if (it->second.filter) p_alDeleteFilters(1, &it->second.filter);
            if (it->second.sendFilter) p_alDeleteFilters(1, &it->second.sendFilter);
        }
    }
    alDeleteSources(1, &it->second.source);
    if (it->second.ownsBuffer) alDeleteBuffers(1, &it->second.buffer);
    mLocalLoops.erase(it);
}

void LLAudioEngine_OpenAL::destroyLocalLoops()
{
    while (!mLocalLoops.empty()) destroyLocalLoop(mLocalLoops.begin()->first);
}

// ---- #892 STREAM SPEAKERS (llaudioengine.h) ----
//
// One timeline of chunks (mStreamChunks, contiguous seq numbers, frame positions), any number of
// streaming sources. A source's AL queue always holds a contiguous run [head, tail) of chunks,
// so AL_SAMPLE_OFFSET (relative to the queue head) + the head chunk's start is its position
// on the timeline. Nothing is queued in streamPush: streamUpdate checks every source's state
// FIRST and queues after, so a stopped source can always be told apart (OpenAL reports every
// buffer of a stopped source as processed, including ones queued after it stopped).

namespace
{
constexpr U32 kStreamPoolCap = 96;         // free AL buffers kept for reuse (more are deleted)
constexpr F64 kStreamStartLead = 0.03;     // group start: this far ahead of the device clock (s)
constexpr F64 kStreamJoinMargin = 0.04;    // a joiner needs a boundary at least this far ahead (s)
constexpr F64 kStreamRestartMin = 0.2;     // a desync restart starts once this much is queued (s)
constexpr F32 kStreamMaxGain = 2.f;       // a speaker's vol= goes to 2 (+6 dB): AL_MAX_GAIN (default 1) raised to it
}

bool LLAudioEngine_OpenAL::streamLoadExt()
{
    if (mStreamExtTried) return p_alSourcePlayAtTimeSOFT != nullptr;
    mStreamExtTried = true;
    p_alGetSourcei64vSOFT = alIsExtensionPresent("AL_SOFT_source_latency")
        ? reinterpret_cast<LPALGETSOURCEI64VSOFT>(alGetProcAddress("alGetSourcei64vSOFT")) : nullptr;
    // (AL_SOFT_source_start_delay may still be listed under an experimental name: the entry
    // points decide.)
    p_alSourcePlayAtTimeSOFT = reinterpret_cast<LPALSOURCEPLAYATTIMESOFT>(alGetProcAddress("alSourcePlayAtTimeSOFT"));
    p_alSourcePlayAtTimevSOFT = reinterpret_cast<LPALSOURCEPLAYATTIMEVSOFT>(alGetProcAddress("alSourcePlayAtTimevSOFT"));
    p_alcGetInteger64vSOFT = (mDevice && alcIsExtensionPresent(mDevice, "ALC_SOFT_device_clock"))
        ? reinterpret_cast<LPALCGETINTEGER64VSOFT>(alcGetProcAddress(mDevice, "alcGetInteger64vSOFT")) : nullptr;
    s_directChannels = alIsExtensionPresent("AL_SOFT_direct_channels") == AL_TRUE;
    alGetError();
    LL_INFOS("ParcelAudio") << "OpenAL stream speakers: start at time " << (p_alSourcePlayAtTimeSOFT && p_alSourcePlayAtTimevSOFT ? "yes" : "no")
                            << ", source latency " << (p_alGetSourcei64vSOFT ? "yes" : "no") << ", device clock "
                            << (p_alcGetInteger64vSOFT ? "yes" : "no") << ", direct channels " << (s_directChannels ? "yes" : "no")
                            << LL_ENDL;
    return p_alSourcePlayAtTimeSOFT != nullptr;
}

bool LLAudioEngine_OpenAL::streamOpen(U32 rate)
{
    if (!mContext || rate == 0) return false;
    if (mStreamOpen)
    {
        if (rate == mStreamRate) return true;
        streamClose();
    }
    streamLoadExt();
    mStreamOpen = true;
    mStreamRate = rate;
    mStreamPlaying = false;
    mStreamPaused = false;
    mStreamRestartNow = false;
    mStreamStartAtNs = 0;
    mStreamLastQueued = 0.0;
    mStreamNextSeq = 0;
    mStreamEnd = 0;
    const U32 peak_src = mStreamStats.sourcesPeak, peak_buf = mStreamStats.buffersPeak;
    mStreamStats = StreamStats();
    mStreamStats.sourcesPeak = peak_src;
    mStreamStats.buffersPeak = peak_buf;
    LL_INFOS("ParcelAudio") << "OpenAL stream timeline open at " << rate << " Hz" << LL_ENDL;
    return true;
}

void LLAudioEngine_OpenAL::streamClose()
{
    while (!mStreamSrcs.empty()) streamDestroySource(mStreamSrcs.begin()->first);
    for (StreamChunk& c : mStreamChunks) streamReleaseChunk(c);
    mStreamChunks.clear();
    if (!mStreamPool.empty())
    {
        alDeleteBuffers((ALsizei)mStreamPool.size(), mStreamPool.data());
        mStreamPool.clear();
    }
    const bool was = mStreamOpen;
    mStreamOpen = false;
    mStreamPlaying = false;
    mStreamPaused = false;
    mStreamBuffersLive = 0;
    alGetError();
    if (was) LL_INFOS("ParcelAudio") << "OpenAL stream timeline closed" << LL_ENDL;
}

LLAudioEngine_OpenAL::StreamChunk* LLAudioEngine_OpenAL::streamChunk(U64 seq)
{
    if (mStreamChunks.empty()) return nullptr;
    const U64 first = mStreamChunks.front().seq;
    if (seq < first || seq - first >= mStreamChunks.size()) return nullptr;
    return &mStreamChunks[(size_t)(seq - first)];
}

U64 LLAudioEngine_OpenAL::streamBoundaryAtOrAfter(U64 pos_frames)
{
    for (const StreamChunk& c : mStreamChunks)
        if (c.start >= pos_frames) return c.seq;
    return mStreamNextSeq;
}

bool LLAudioEngine_OpenAL::streamPush(const S16* const* pcm, U32 frames)
{
    if (!mStreamOpen || frames == 0) return false;
    if (streamQueuedSeconds() >= kStreamMaxQueueSeconds)
    {
        ++mStreamStats.pushRejected;
        return false;
    }
    StreamChunk c;
    c.seq = mStreamNextSeq++;
    c.start = mStreamEnd;
    c.frames = frames;
    for (U32 r = 0; r < STREAM_ROLE_COUNT; ++r)
    {
        if (pcm && pcm[r])
        {
            const size_t n = (size_t)frames * (r == STREAM_STEREO ? 2 : 1);
            c.pcm[r].assign(pcm[r], pcm[r] + n);
        }
    }
    mStreamEnd += frames;
    mStreamChunks.push_back(std::move(c));
    return true;
}

void LLAudioEngine_OpenAL::streamReleaseChunk(StreamChunk& c)
{
    for (U32 r = 0; r < STREAM_ROLE_COUNT; ++r)
    {
        if (!c.buf[r]) continue;
        if (mStreamPool.size() < kStreamPoolCap) mStreamPool.push_back(c.buf[r]);
        else alDeleteBuffers(1, &c.buf[r]);
        c.buf[r] = 0;
        c.refs[r] = 0;
        if (mStreamBuffersLive) --mStreamBuffersLive;
    }
}

bool LLAudioEngine_OpenAL::streamQueueOn(StreamSrc& s, U64 upto)
{
    static std::vector<S16> s_silence;
    const U32 ch = s.role == STREAM_STEREO ? 2 : 1;
    const ALenum fmt = ch == 2 ? AL_FORMAT_STEREO16 : AL_FORMAT_MONO16;
    while (s.tail < upto)
    {
        StreamChunk* c = streamChunk(s.tail);
        if (!c) return false;
        ALuint& b = c->buf[s.role];
        if (!b)
        {
            alGetError();
            if (!mStreamPool.empty())
            {
                b = mStreamPool.back();
                mStreamPool.pop_back();
            }
            else
            {
                alGenBuffers(1, &b);
                if (alGetError() != AL_NO_ERROR)
                {
                    b = 0;
                    return false;
                }
            }
            ++mStreamBuffersLive;
            mStreamStats.buffersPeak = std::max(mStreamStats.buffersPeak, mStreamBuffersLive);
            const std::vector<S16>& v = c->pcm[s.role];
            const size_t n = (size_t)c->frames * ch;
            const S16* data = v.data();
            if (v.size() < n)
            {
                if (s_silence.size() < n) s_silence.assign(n, 0);
                data = s_silence.data();
            }
            alBufferData(b, fmt, data, (ALsizei)(n * sizeof(S16)), (ALsizei)mStreamRate);
            if (alGetError() != AL_NO_ERROR)
            {
                alDeleteBuffers(1, &b);
                b = 0;
                --mStreamBuffersLive;
                alGetError();
                return false;
            }
        }
        alGetError();
        alSourceQueueBuffers(s.source, 1, &b);
        if (alGetError() != AL_NO_ERROR) return false;
        ++c->refs[s.role];
        ++s.tail;
    }
    return true;
}

void LLAudioEngine_OpenAL::streamUnqueueProcessed(StreamSrc& s)
{
    ALint n = 0;
    alGetSourcei(s.source, AL_BUFFERS_PROCESSED, &n);
    while (n-- > 0 && s.head < s.tail)
    {
        ALuint b = 0;
        alSourceUnqueueBuffers(s.source, 1, &b);
        if (StreamChunk* c = streamChunk(s.head))
            if (c->refs[s.role]) --c->refs[s.role];
        ++s.head;
    }
}

void LLAudioEngine_OpenAL::streamUnqueueAll(StreamSrc& s)
{
    alSourceStop(s.source);
    alSourcei(s.source, AL_BUFFER, AL_NONE);   // (a stopped source: drops its whole queue)
    alSourceRewind(s.source);                  // -> AL_INITIAL: "stopped" means ran dry from now on
    s.startAtNs = 0;
    for (U64 q = s.head; q < s.tail; ++q)
        if (StreamChunk* c = streamChunk(q))
            if (c->refs[s.role]) --c->refs[s.role];
    s.head = s.tail;
    alGetError();
}

void LLAudioEngine_OpenAL::streamDropPlayed()
{
    // Before the group runs, everything is kept (pre-buffer; streamPush bounds it).
    if (!mStreamPlaying) return;
    while (!mStreamChunks.empty())
    {
        StreamChunk& c = mStreamChunks.front();
        bool held = false;
        for (U32 r = 0; r < STREAM_ROLE_COUNT; ++r) held |= c.refs[r] != 0;
        if (held) break;
        // Still to be queued by a running (or waiting) source?
        bool needed = false;
        for (const auto& kv : mStreamSrcs)
            if (kv.second.state != StreamSrc::JOINING && kv.second.tail <= c.seq) needed = true;
        if (needed) break;
        streamReleaseChunk(c);
        mStreamChunks.pop_front();
    }
}

bool LLAudioEngine_OpenAL::streamPlayhead(U64& pos_frames, S64* clock_ns, F64* frac)
{
    // The reference is the stereo bed when it plays: it is listener-relative with no velocity, so
    // OpenAL never doppler-shifts it -- it IS the timeline. A positioned speaker is doppler-shifted
    // whenever the listener moves (AL_VELOCITY = the agent's), drifts off the timeline by the
    // change in its distance / 343 m/s, and must never be what a joiner aligns to.
    for (int pass = 0; pass < 2; ++pass)
    for (auto& kv : mStreamSrcs)
    {
        StreamSrc& s = kv.second;
        if ((pass == 0) != (s.role == STREAM_STEREO)) continue;
        if (s.state != StreamSrc::ACTIVE || s.head >= s.tail) continue;
        ALint st = AL_STOPPED;
        alGetSourcei(s.source, AL_SOURCE_STATE, &st);
        if (st != AL_PLAYING && st != AL_PAUSED) continue;
        const StreamChunk* c = streamChunk(s.head);
        if (!c) continue;
        U64 off = 0;
        S64 clk = -1;
        F64 fr = 0.0;
        if (p_alGetSourcei64vSOFT)
        {
            ALint64SOFT v[2] = { 0, 0 };
            p_alGetSourcei64vSOFT(s.source, AL_SAMPLE_OFFSET_CLOCK_SOFT, v);
            if (v[0] < 0) v[0] = 0;
            off = (U64)(v[0] >> 32);
            fr = (F64)(v[0] & 0xffffffffLL) / 4294967296.0;
            clk = v[1];
        }
        else
        {
            ALint o = 0;
            alGetSourcei(s.source, AL_SAMPLE_OFFSET, &o);
            off = (U64)std::max(o, 0);
        }
        pos_frames = c->start + off;
        if (clock_ns) *clock_ns = clk;
        if (frac) *frac = fr;
        return true;
    }
    return false;
}

S64 LLAudioEngine_OpenAL::streamClockNs()
{
    if (!p_alcGetInteger64vSOFT || !mDevice) return -1;
    ALCint64SOFT ns = 0;
    p_alcGetInteger64vSOFT(mDevice, ALC_DEVICE_CLOCK_SOFT, 1, &ns);
    return (S64)ns;
}

bool LLAudioEngine_OpenAL::streamRunning(const StreamSrc& s, S64 now_ns)
{
    ALint st = AL_STOPPED;
    alGetSourcei(s.source, AL_SOURCE_STATE, &st);
    if (st == AL_PLAYING) return true;
    // A start scheduled with PlayAtTime that has not begun yet is not a stop.
    return st == AL_INITIAL && s.startAtNs > 0 && now_ns >= 0 && now_ns < s.startAtNs + 250000000LL;
}

F64 LLAudioEngine_OpenAL::streamQueuedSeconds()
{
    if (!mStreamOpen || !mStreamRate) return 0.0;
    U64 pos = 0;
    if (mStreamPlaying && streamPlayhead(pos, nullptr, nullptr))
        return pos >= mStreamEnd ? 0.0 : (F64)(mStreamEnd - pos) / (F64)mStreamRate;
    if (mStreamChunks.empty()) return 0.0;
    return (F64)(mStreamEnd - mStreamChunks.front().start) / (F64)mStreamRate;
}

void LLAudioEngine_OpenAL::streamRestartAll(U64 resume_seq, bool underrun)
{
    std::vector<ALuint> ids;
    for (auto& kv : mStreamSrcs) ids.push_back(kv.second.source);
    if (!ids.empty()) alSourceStopv((ALsizei)ids.size(), ids.data());
    for (auto& kv : mStreamSrcs) streamUnqueueAll(kv.second);
    while (!mStreamChunks.empty() && mStreamChunks.front().seq < resume_seq)
    {
        streamReleaseChunk(mStreamChunks.front());
        mStreamChunks.pop_front();
    }
    const U64 from = mStreamChunks.empty() ? mStreamNextSeq : mStreamChunks.front().seq;
    for (auto& kv : mStreamSrcs)
    {
        kv.second.state = StreamSrc::WAITING;
        kv.second.head = kv.second.tail = from;
    }
    mStreamPlaying = false;
    mStreamRestartNow = !underrun;
    if (underrun) ++mStreamStats.underruns;
    else ++mStreamStats.restarts;
    alGetError();
}

void LLAudioEngine_OpenAL::streamStartGroup()
{
    std::vector<ALuint> ids;
    for (auto& kv : mStreamSrcs)
        if (kv.second.state == StreamSrc::WAITING && kv.second.head < kv.second.tail) ids.push_back(kv.second.source);
    if (ids.empty()) return;
    alGetError();
    bool started = false;
    if (p_alSourcePlayAtTimevSOFT && p_alcGetInteger64vSOFT && mDevice)
    {
        ALCint64SOFT now = 0;
        p_alcGetInteger64vSOFT(mDevice, ALC_DEVICE_CLOCK_SOFT, 1, &now);
        const S64 at = (S64)now + (S64)(kStreamStartLead * 1e9);
        p_alSourcePlayAtTimevSOFT((ALsizei)ids.size(), ids.data(), at);
        started = alGetError() == AL_NO_ERROR;
        if (started) mStreamStartAtNs = at;
    }
    if (!started)
    {
        // One call: OpenAL Soft starts every listed source on the same mixer update.
        alSourcePlayv((ALsizei)ids.size(), ids.data());
        started = alGetError() == AL_NO_ERROR;
        mStreamStartAtNs = 0;
    }
    if (!started) return;
    for (auto& kv : mStreamSrcs)
    {
        StreamSrc& s = kv.second;
        if (s.state != StreamSrc::WAITING) continue;
        if (s.head < s.tail)
        {
            s.state = StreamSrc::ACTIVE;
            s.startAtNs = mStreamStartAtNs;
        }
        else
        {
            // (Nothing queued -- a failed buffer: it joins the running group instead.)
            streamUnqueueAll(s);
            s.state = StreamSrc::JOINING;
            s.head = s.tail = 0;
        }
    }
    mStreamPlaying = true;
    mStreamRestartNow = false;
    ++mStreamStats.starts;
}

LLAudioEngine_OpenAL::EJoin LLAudioEngine_OpenAL::streamJoin(StreamSrc& s)
{
    if (!p_alSourcePlayAtTimeSOFT || !p_alGetSourcei64vSOFT) return JOIN_UNSUPPORTED;
    U64 pos = 0;
    S64 clk = -1;
    F64 frac = 0.0;
    if (!streamPlayhead(pos, &clk, &frac) || clk <= 0) return JOIN_WAIT;
    if (clk < mStreamStartAtNs) return JOIN_WAIT;   // the group has not actually started yet
    ALint st = AL_STOPPED;
    // (The reference is paused: wait for the resume.)
    for (auto& kv : mStreamSrcs)
        if (kv.second.state == StreamSrc::ACTIVE) { alGetSourcei(kv.second.source, AL_SOURCE_STATE, &st); break; }
    if (st != AL_PLAYING) return JOIN_WAIT;
    const U64 j = streamBoundaryAtOrAfter(pos + (U64)(kStreamJoinMargin * mStreamRate));
    const StreamChunk* c = streamChunk(j);
    if (!c) return JOIN_WAIT;   // nothing queued that far ahead yet
    s.head = s.tail = j;
    if (!streamQueueOn(s, mStreamNextSeq) || s.head >= s.tail)
    {
        streamUnqueueAll(s);
        s.head = s.tail = 0;
        return JOIN_WAIT;
    }
    const F64 ahead = ((F64)(c->start - pos) - frac) / (F64)mStreamRate;   // seconds after `clk`
    const S64 at = clk + (S64)(ahead * 1e9);
    alGetError();
    p_alSourcePlayAtTimeSOFT(s.source, at);
    if (alGetError() != AL_NO_ERROR)
    {
        streamUnqueueAll(s);
        s.head = s.tail = 0;
        return JOIN_UNSUPPORTED;
    }
    s.state = StreamSrc::ACTIVE;
    s.startAtNs = at;
    s.joinChecked = false;     // (the sync guard reports how exact it landed)
    ++mStreamStats.lateJoins;
    return JOIN_DONE;
}

void LLAudioEngine_OpenAL::streamUpdate(F64 prebuffer_secs)
{
    if (!mStreamOpen) return;
    if (mStreamPaused)
    {
        mStreamLastQueued = streamQueuedSeconds();
        return;
    }

    // 1) Every running source still running? (Checked before anything new is queued.)
    if (mStreamPlaying)
    {
        bool any_running = false, any_stopped = false, any_active = false;
        U64 played_to = mStreamNextSeq;
        const S64 now_ns = streamClockNs();
        for (auto& kv : mStreamSrcs)
        {
            StreamSrc& s = kv.second;
            if (s.state != StreamSrc::ACTIVE) continue;
            any_active = true;
            if (streamRunning(s, now_ns)) any_running = true;
            else
            {
                any_stopped = true;
                played_to = std::min(played_to, s.tail);   // it heard everything it had queued
            }
        }
        if (!any_active)
        {
            mStreamPlaying = false;   // (every source went away: pre-buffer for the next ones)
            for (auto& kv : mStreamSrcs)
            {
                if (kv.second.state != StreamSrc::JOINING) continue;
                kv.second.state = StreamSrc::WAITING;
                kv.second.head = kv.second.tail = mStreamChunks.empty() ? mStreamNextSeq : mStreamChunks.front().seq;
            }
        }
        else if (any_stopped)
        {
            U64 pos = 0;
            if (any_running && streamPlayhead(pos, nullptr, nullptr))
            {
                // A desync (one stopped while others play): all restart together at the next boundary.
                LL_INFOS("ParcelAudio") << "stream speakers: a source stopped while the others played; restarting all together"
                                        << LL_ENDL;
                streamRestartAll(streamBoundaryAtOrAfter(pos), false);
            }
            else
            {
                LL_INFOS("ParcelAudio") << "stream speakers: underrun (every source ran dry); pre-buffering again" << LL_ENDL;
                streamRestartAll(played_to, true);
            }
        }
    }

    // 2) Reclaim what has been played.
    for (auto& kv : mStreamSrcs)
        if (kv.second.state == StreamSrc::ACTIVE) streamUnqueueProcessed(kv.second);
    streamDropPlayed();

    // 3) Queue the new chunks on every running / waiting source. A running source that is
    // stopped right after this had run dry BEFORE these arrived: they are unheard.
    for (auto& kv : mStreamSrcs)
    {
        StreamSrc& s = kv.second;
        if (s.state == StreamSrc::JOINING) continue;
        const U64 first_new = s.tail;
        streamQueueOn(s, mStreamNextSeq);
        if (s.state == StreamSrc::ACTIVE && mStreamPlaying && first_new < s.tail)
        {
            if (!streamRunning(s, streamClockNs()))
            {
                LL_INFOS("ParcelAudio") << "stream speakers: underrun (ran dry just before new audio); pre-buffering again"
                                        << LL_ENDL;
                streamRestartAll(first_new, true);
                // (every source is WAITING now; the loop below queues them again)
                for (auto& kv2 : mStreamSrcs) streamQueueOn(kv2.second, mStreamNextSeq);
                break;
            }
        }
    }

    // 4) Start the group once pre-buffered (a desync restart needs only a little).
    if (!mStreamPlaying)
    {
        const F64 q = streamQueuedSeconds();
        if (q >= prebuffer_secs || (mStreamRestartNow && q >= kStreamRestartMin)) streamStartGroup();
    }
    // 5) Join late sources at a boundary, sample-exact; without PlayAtTime, restart the group.
    else
    {
        for (auto& kv : mStreamSrcs)
        {
            StreamSrc& s = kv.second;
            if (s.state != StreamSrc::JOINING) continue;
            const EJoin r = streamJoin(s);
            if (r == JOIN_UNSUPPORTED)
            {
                U64 pos = 0;
                const U64 resume = streamPlayhead(pos, nullptr, nullptr) ? streamBoundaryAtOrAfter(pos) : mStreamNextSeq;
                streamRestartAll(resume, false);
                --mStreamStats.restarts;    // (counted as a join fallback instead)
                ++mStreamStats.joinFallbacks;
                for (auto& kv2 : mStreamSrcs) streamQueueOn(kv2.second, mStreamNextSeq);
                if (streamQueuedSeconds() >= kStreamRestartMin) streamStartGroup();
                break;
            }
        }
    }
    streamSyncGuard();
    mStreamLastQueued = streamQueuedSeconds();
    alGetError();   // (leave no error behind for the engine's own checks)
}

void LLAudioEngine_OpenAL::streamSyncGuard()
{
    // ~1 Hz: where is every playing source on the timeline right now? AL_SAMPLE_OFFSET_CLOCK_SOFT
    // gives what the mixer actually played (queue head chunk + offset) and the device time of that
    // position; all are brought to one instant and compared with the stereo bed (no doppler: the
    // timeline). A speaker off by more than the tolerance is re-joined at the next chunk boundary
    // (a short gap in that one speaker). Doppler is kept: while the listener moves, a speaker
    // legitimately drifts (distance change / speed of sound), so small drifts wait until the
    // listener slows down; only a large one (> 50 ms) is fixed on the move.
    if (!mStreamPlaying || mStreamPaused || !p_alGetSourcei64vSOFT || !mStreamRate) return;
    const S64 now = streamClockNs();
    if (now < 0 || now < mStreamSyncNextNs) return;
    mStreamSyncNextNs = now + 1000000000LL;

    struct Meas { StreamSrc* s; F64 pos; S64 clk; };
    static std::vector<Meas> ms;
    ms.clear();
    S64 tmax = 0;
    for (auto& kv : mStreamSrcs)
    {
        StreamSrc& s = kv.second;
        if (s.state != StreamSrc::ACTIVE || s.head >= s.tail) continue;
        if (s.startAtNs > 0 && now < s.startAtNs + 100000000LL) continue;   // (its start is not 100 ms old)
        ALint st = AL_STOPPED;
        alGetSourcei(s.source, AL_SOURCE_STATE, &st);
        if (st != AL_PLAYING) continue;
        const StreamChunk* c = streamChunk(s.head);
        if (!c) continue;
        ALint64SOFT v[2] = { 0, 0 };
        p_alGetSourcei64vSOFT(s.source, AL_SAMPLE_OFFSET_CLOCK_SOFT, v);
        if (v[1] <= 0 || v[0] < 0) continue;
        const F64 pos = (F64)c->start + (F64)(v[0] >> 32) + (F64)(v[0] & 0xffffffffLL) / 4294967296.0;
        ms.push_back({ &s, pos, (S64)v[1] });
        tmax = std::max(tmax, (S64)v[1]);
    }
    if (ms.size() < 2) return;
    // One instant (the clocks differ by at most a mixer update; the rate error over that is nil).
    for (Meas& m : ms) m.pos += (F64)(tmax - m.clk) * 1e-9 * (F64)mStreamRate;
    const Meas* bed = nullptr;
    for (const Meas& m : ms)
        if (m.s->role == STREAM_STEREO) { bed = &m; break; }
    F64 ref = 0.0;
    if (bed) ref = bed->pos;
    else
    {
        std::vector<F64> p;
        for (const Meas& m : ms) p.push_back(m.pos);
        std::nth_element(p.begin(), p.begin() + p.size() / 2, p.end());
        ref = p[p.size() / 2];
    }
    const LLVector3 lvel = mListenerp ? mListenerp->getVelocity() : LLVector3::zero;
    const bool moving = lvel.magVecSquared() > 1.f;   // > 1 m/s
    F32 lo = 1e9f, hi = -1e9f;
    for (const Meas& m : ms)
    {
        StreamSrc& s = *m.s;
        if (&m == bed) continue;
        const F32 off_ms = (F32)((m.pos - ref) * 1000.0 / (F64)mStreamRate);   // + = ahead of the timeline
        lo = std::min(lo, off_ms);
        hi = std::max(hi, off_ms);
        mStreamSyncWinOffset = std::max(mStreamSyncWinOffset, std::fabs(off_ms));
        if (!s.joinChecked)
        {
            s.joinChecked = true;
            mStreamSyncWinJoin = std::max(mStreamSyncWinJoin, std::fabs(off_ms));
            if (std::fabs(off_ms) > 5.f)
                LL_INFOS("ParcelAudio") << "stream speakers: a joined source landed " << llformat("%+.1f", off_ms)
                                        << " ms off the timeline" << LL_ENDL;
        }
        const F32 tol = mStreamSyncTolMs;
        if (tol <= 0.f || std::fabs(off_ms) <= tol) continue;
        if (moving && s.applied.doppler && std::fabs(off_ms) <= 50.f)
        {
            ++mStreamStats.syncDeferred;   // (doppler on the move: fixed once the listener slows)
            continue;
        }
        if (s.lastResyncNs > 0 && now - s.lastResyncNs < 3000000000LL) continue;   // (one try per 3 s)
        if (std::fabs(off_ms) > 50.f || !s.applied.doppler)
            LL_INFOS("ParcelAudio") << "stream speakers: a source was " << llformat("%+.1f", off_ms)
                                    << " ms off the timeline" << (s.applied.doppler ? "" : " WITH DOPPLER OFF (should not drift)")
                                    << ": re-joined at the next chunk boundary" << LL_ENDL;
        streamUnqueueAll(s);
        s.state = StreamSrc::JOINING;
        s.head = s.tail = 0;
        s.lastResyncNs = now;
        ++mStreamStats.syncResyncs;
    }
    if (hi >= lo) mStreamSyncWinSpread = std::max(mStreamSyncWinSpread, hi - lo);
    ++mStreamStats.syncChecks;
    if (++mStreamSyncWinChecks >= 10)
    {
        mStreamStats.syncOffsetMaxMs = mStreamSyncWinOffset;
        mStreamStats.syncSpreadMaxMs = mStreamSyncWinSpread;
        mStreamStats.syncJoinErrMaxMs = mStreamSyncWinJoin;
        mStreamSyncWinChecks = 0;
        mStreamSyncWinOffset = mStreamSyncWinSpread = mStreamSyncWinJoin = 0.f;
    }
}

void LLAudioEngine_OpenAL::streamFlush()
{
    if (!mStreamOpen) return;
    std::vector<ALuint> ids;
    for (auto& kv : mStreamSrcs) ids.push_back(kv.second.source);
    if (!ids.empty()) alSourceStopv((ALsizei)ids.size(), ids.data());
    for (auto& kv : mStreamSrcs) streamUnqueueAll(kv.second);
    for (StreamChunk& c : mStreamChunks) streamReleaseChunk(c);
    mStreamChunks.clear();
    for (auto& kv : mStreamSrcs)
    {
        kv.second.state = StreamSrc::WAITING;
        kv.second.head = kv.second.tail = mStreamNextSeq;
    }
    mStreamPlaying = false;
    mStreamRestartNow = false;
    ++mStreamStats.flushes;
    alGetError();
}

void LLAudioEngine_OpenAL::streamPause(bool pause)
{
    if (!mStreamOpen || pause == mStreamPaused) return;
    mStreamPaused = pause;
    if (!mStreamPlaying) return;   // (pre-buffering: the start waits for the resume)
    std::vector<ALuint> ids;
    for (auto& kv : mStreamSrcs)
        if (kv.second.state == StreamSrc::ACTIVE) ids.push_back(kv.second.source);
    if (ids.empty()) return;
    // One call each way: every source pauses / resumes on the same mixer update.
    if (pause) alSourcePausev((ALsizei)ids.size(), ids.data());
    else alSourcePlayv((ALsizei)ids.size(), ids.data());
    mStreamStartAtNs = 0;
    alGetError();
}

U32 LLAudioEngine_OpenAL::streamCreateSource(EStreamRole role)
{
    if (!mStreamOpen || role >= STREAM_ROLE_COUNT) return 0;
    alGetError();
    StreamSrc s;
    alGenSources(1, &s.source);
    if (alGetError() != AL_NO_ERROR || !s.source) return 0;
    s.role = role;
    alSourcei(s.source, AL_LOOPING, AL_FALSE);
    alSourcei(s.source, AL_BUFFER, AL_NONE);
    alSourcef(s.source, AL_GAIN, 0.f);
    alSource3f(s.source, AL_VELOCITY, 0.f, 0.f, 0.f);
    // Reverb level: with AL_AUXILIARY_SEND_FILTER_GAIN_AUTO (the default) OpenAL Soft scales the
    // wet path by the reverb's initial decay over (distance - AL_REFERENCE_DISTANCE) x
    // AL_ROLLOFF_FACTOR, and by the cone. A channel has rolloff 1 and reference 1 m, so that is
    // the true distance; the speakers had rolloff ~10 (their old -30 dB at r), so their wet path
    // decayed as if ~10x farther: 15 m past the reference with a 1.5 s decay was -18 dB of reverb
    // where a channel loses -1.6 dB -- the speakers sounded much drier than the sounds around
    // them, except one inside its reference distance. (They now use rolloff 1 too, but a
    // reference of up to 4 m, and the cone.) Off here: streamApplyEfx applies the channels'
    // decay (true distance past 1 m, listener rolloff) to the send gains itself, or none at all
    // for a diffuse-field speaker (StreamSourceParams::diffuse).
    alSourcei(s.source, AL_AUXILIARY_SEND_FILTER_GAIN_AUTO, AL_FALSE);
    // OpenAL clamps a source's gain to its AL_MAX_GAIN (default 1): a vol=2 speaker would stop at 1.
    alSourcef(s.source, AL_MAX_GAIN, kStreamMaxGain * kAcousticsMaxBoost);   // (x the #683 early-gain boost headroom)
    if (role == STREAM_STEREO)
    {
        // The bed: at the listener, L to the left and R to the right as they are (no virtual
        // speakers, no HRTF), like the plugin's own output.
        alSourcei(s.source, AL_SOURCE_RELATIVE, AL_TRUE);
        alSource3f(s.source, AL_POSITION, 0.f, 0.f, 0.f);
        alSourcef(s.source, AL_ROLLOFF_FACTOR, 0.f);
        if (s_directChannels) alSourcei(s.source, AL_DIRECT_CHANNELS_SOFT, AL_TRUE);
    }
    alGetError();
    if (mStreamPlaying)
    {
        s.state = StreamSrc::JOINING;
        s.head = s.tail = 0;
    }
    else
    {
        s.state = StreamSrc::WAITING;
        s.head = s.tail = mStreamChunks.empty() ? mStreamNextSeq : mStreamChunks.front().seq;
    }
    const U32 id = mStreamNextId++;
    mStreamSrcs[id] = s;
    mStreamStats.sourcesPeak = std::max(mStreamStats.sourcesPeak, (U32)mStreamSrcs.size());
    return id;
}

void LLAudioEngine_OpenAL::streamDestroySource(U32 id)
{
    auto it = mStreamSrcs.find(id);
    if (it == mStreamSrcs.end()) return;
    StreamSrc& s = it->second;
    streamUnqueueAll(s);
    streamDetachEfx(s, true);
    alDeleteSources(1, &s.source);
    alGetError();
    mStreamSrcs.erase(it);
}

void LLAudioEngine_OpenAL::streamSetSource(U32 id, const StreamSourceParams& p)
{
    auto it = mStreamSrcs.find(id);
    if (it == mStreamSrcs.end()) return;
    StreamSrc& s = it->second;
    const bool efx = p.efx && efxReady();
    // Without EFX the low-pass cannot exist: fold a part of the HF cut into the level (as the
    // thunder shots do).
    const F32 direct = (p.efx && !efx) ? llclamp(p.directGain, 0.f, 1.f) * (0.5f + 0.5f * llclamp(p.directHF, 0.f, 1.f)) : 1.f;
    // #683 early gain: the boost rides on the source gain (the sends below are divided by it).
    const F32 boost = (p.efx && efx && !p.relative) ? llclamp(p.directBoost, 1.f, kAcousticsMaxBoost) : 1.f;
    const F32 gain = llclamp(p.gain * direct * boost, 0.f, kStreamMaxGain * boost);
    const StreamSourceParams& a = s.applied;
    const bool first = !s.appliedValid;
    if (first || std::fabs(gain - a.gain) > 1e-4f) alSourcef(s.source, AL_GAIN, gain);
    if (s.role != STREAM_STEREO)
    {
        // No doppler: head-relative in the listener frame, every call (the listener moves / turns).
        const bool head_rel = p.relative || !p.doppler;
        const bool mode_changed = first || head_rel != s.headRel || p.relative != a.relative;
        if (mode_changed)
        {
            alSourcei(s.source, AL_SOURCE_RELATIVE, head_rel ? AL_TRUE : AL_FALSE);
            s.headRel = head_rel;
        }
        if (p.relative)
        {
            if (mode_changed) alSource3f(s.source, AL_POSITION, 0.f, 0.f, 0.f);
        }
        else if (!p.doppler)
        {
            const LLVector3 rel = toListenerFrame(p.pos, true);
            alSourcefv(s.source, AL_POSITION, rel.mV);
        }
        else if (mode_changed || p.pos != a.pos)
        {
            alSourcefv(s.source, AL_POSITION, p.pos.mV);
        }
        const F32 ref = std::max(p.refDist, 0.1f);
        const F32 maxd = std::max(p.maxDist, ref);
        if (first || ref != a.refDist || maxd != a.maxDist || p.rolloff != a.rolloff)
        {
            alSourcef(s.source, AL_REFERENCE_DISTANCE, ref);
            alSourcef(s.source, AL_MAX_DISTANCE, maxd);
            alSourcef(s.source, AL_ROLLOFF_FACTOR, std::max(p.rolloff, 0.f));
        }
        const bool cone = p.cone && !p.relative;
        if (cone)
        {
            if (!p.doppler)
            {
                const LLVector3 d = toListenerFrame(p.dir, false);   // (the cone turns with the listener frame)
                alSourcefv(s.source, AL_DIRECTION, d.mV);
            }
            if (mode_changed || !a.cone || p.dir != a.dir || p.coneInner != a.coneInner || p.coneOuter != a.coneOuter
                || p.coneOuterGain != a.coneOuterGain)
            {
                if (p.doppler) alSourcefv(s.source, AL_DIRECTION, p.dir.mV);
                alSourcef(s.source, AL_CONE_INNER_ANGLE, llclamp(p.coneInner, 0.f, 360.f));
                alSourcef(s.source, AL_CONE_OUTER_ANGLE, llclamp(p.coneOuter, 0.f, 360.f));
                alSourcef(s.source, AL_CONE_OUTER_GAIN, llclamp(p.coneOuterGain, 0.f, 1.f));
            }
        }
        else if (first || a.cone)
        {
            alSource3f(s.source, AL_DIRECTION, 0.f, 0.f, 0.f);   // omnidirectional
            alSourcef(s.source, AL_CONE_INNER_ANGLE, 360.f);
            alSourcef(s.source, AL_CONE_OUTER_ANGLE, 360.f);
        }
    }
    s.applied = p;
    s.applied.gain = gain;
    s.appliedValid = true;
    if (efx) streamApplyEfx(s, p);
    else streamDetachEfx(s, false);
    alGetError();
}

LLVector3 LLAudioEngine_OpenAL::toListenerFrame(const LLVector3& v, bool point)
{
    // OpenAL Soft's listener matrix (alc/alu.cpp: N = at, V = up, U = N x V, V = U x N; rows U, V,
    // -N): a head-relative source at M (P - L) renders exactly as a world one at P -- same distance,
    // direction and cone -- but without the doppler a world source gets from the listener velocity.
    if (!mListenerp) return v;
    LLVector3 n = mListenerp->getAt();
    LLVector3 up = mListenerp->getUp();
    if (n.normVec() <= 0.f || up.normVec() <= 0.f) return v;
    LLVector3 u = n % up;   // (cross product)
    if (u.normVec() <= 0.f) return v;
    up = u % n;
    const LLVector3 d = point ? v - mListenerp->getPosition() : v;
    return LLVector3(d * u, d * up, -(d * n));
}

void LLAudioEngine_OpenAL::streamApplyEfx(StreamSrc& s, const StreamSourceParams& p)
{
    // #683 early gain: the source gain carries the boost (streamApply), so the sends divide it out.
    const F32 boost = !p.relative ? llclamp(p.directBoost, 1.f, kAcousticsMaxBoost) : 1.f;
    alGetError();
    for (ALuint* fp : { &s.filter, &s.sendFilter, &s.zoneFilter })
    {
        if (*fp) continue;
        p_alGenFilters(1, fp);
        if (alGetError() != AL_NO_ERROR || !*fp)
        {
            *fp = 0;
            return;
        }
        p_alFilteri(*fp, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
        if (alGetError() != AL_NO_ERROR)
        {
            p_alDeleteFilters(1, fp);
            *fp = 0;
            return;
        }
    }
    // #897 boundary bass (send 2 -> the EQ slot; not for the listener-relative bed).
    const ALuint bslot = (!p.relative && p.bassBoostDb > 0.05f && mAcSends >= 3) ? acousticsBassSlot() : 0;
    if (bslot && !s.bassFilter)
    {
        p_alGenFilters(1, &s.bassFilter);
        if (alGetError() != AL_NO_ERROR || !s.bassFilter) s.bassFilter = 0;
        else
        {
            p_alFilteri(s.bassFilter, AL_FILTER_TYPE, AL_FILTER_LOWPASS);
            if (alGetError() != AL_NO_ERROR)
            {
                p_alDeleteFilters(1, &s.bassFilter);
                s.bassFilter = 0;
            }
        }
    }
    F32 bass_send = 0.f, bass_mul = 1.f;
    if (bslot && s.bassFilter) bassSend(p.bassBoostDb, bass_send, bass_mul);
    // (A source copies a filter's parameters when it is attached: re-attach on change.)
    const F32 g = llclamp(p.directGain * bass_mul, 0.f, 1.f), hf = llclamp(p.directHF, 0.f, 1.f);
    if (!s.directAttached || std::fabs(g - s.appliedDirect) > 0.003f || std::fabs(hf - s.appliedHF) > 0.003f)
    {
        p_alFilterf(s.filter, AL_LOWPASS_GAIN, g);
        p_alFilterf(s.filter, AL_LOWPASS_GAINHF, hf);
        alSourcei(s.source, AL_DIRECT_FILTER, (ALint)s.filter);
        s.directAttached = true;
        s.appliedDirect = g;
        s.appliedHF = hf;
    }
    // Send 0: the listener's room.
    if (mAcOn && mAcSlot)
    {
        F32 wg = 1.f, whf = 1.f;
        if (!p.diffuse && mAcAppliedValid) streamWetDecay(p, mAcApplied, wg, whf);
        const F32 sg = llclamp(p.sendGain * wg / boost, 0.f, 1.f), shf = llclamp(p.sendHF * whf, 0.f, 1.f);
        if (!s.sendAttached || std::fabs(sg - s.appliedSend) > 0.003f || std::fabs(shf - s.appliedSendHF) > 0.003f)
        {
            p_alFilterf(s.sendFilter, AL_LOWPASS_GAIN, sg);
            p_alFilterf(s.sendFilter, AL_LOWPASS_GAINHF, shf);
            alSource3i(s.source, AL_AUXILIARY_SEND_FILTER, (ALint)mAcSlot, 0, (ALint)s.sendFilter);
            s.sendAttached = true;
            s.appliedSend = sg;
            s.appliedSendHF = shf;
        }
    }
    else if (s.sendAttached)
    {
        alSource3i(s.source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
        s.sendAttached = false;
        s.appliedSend = s.appliedSendHF = -1.f;
    }
    // Send 1: the speaker's own room as a reverb zone (#857), when it has one.
    const ALuint zslot = mAcSends >= 2 ? acousticsZoneSlot(p.zone) : 0;
    F32 zwg = 1.f, zwhf = 1.f;
    if (!p.diffuse && zslot && mAcZones[p.zone].applied_valid) streamWetDecay(p, mAcZones[p.zone].applied, zwg, zwhf);
    const F32 zg = llclamp(p.zoneGain * zwg / boost, 0.f, 1.f), zhf = llclamp(p.zoneHF * zwhf, 0.f, 1.f);
    if (zslot && zg > 0.f)
    {
        if (s.zoneSlotAttached != zslot || std::fabs(zg - s.appliedZone) > 0.003f || std::fabs(zhf - s.appliedZoneHF) > 0.003f)
        {
            p_alFilterf(s.zoneFilter, AL_LOWPASS_GAIN, zg);
            p_alFilterf(s.zoneFilter, AL_LOWPASS_GAINHF, zhf);
            alSource3i(s.source, AL_AUXILIARY_SEND_FILTER, (ALint)zslot, 1, (ALint)s.zoneFilter);
            s.zoneSlotAttached = zslot;
            s.appliedZone = zg;
            s.appliedZoneHF = zhf;
        }
    }
    else if (s.zoneSlotAttached)
    {
        alSource3i(s.source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 1, AL_FILTER_NULL);
        s.zoneSlotAttached = 0;
        s.appliedZone = s.appliedZoneHF = -1.f;
    }
    // Send 2: the boundary bass (#897), with the dry path's distance / cone attenuation (its send
    // gets none from OpenAL) and the occlusion gain.
    if (bass_send > 0.f)
    {
        const F32 bg = llclamp(bass_send * llclamp(p.directGain, 0.f, 1.f) * streamDryAttn(p), 0.f, 1.f);
        if (s.bassSlotAttached != bslot || std::fabs(bg - s.appliedBass) > 0.003f)
        {
            p_alFilterf(s.bassFilter, AL_LOWPASS_GAIN, bg);
            p_alFilterf(s.bassFilter, AL_LOWPASS_GAINHF, 0.f);
            alSource3i(s.source, AL_AUXILIARY_SEND_FILTER, (ALint)bslot, 2, (ALint)s.bassFilter);
            s.bassSlotAttached = bslot;
            s.appliedBass = bg;
        }
    }
    else if (s.bassSlotAttached)
    {
        alSource3i(s.source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 2, AL_FILTER_NULL);
        s.bassSlotAttached = 0;
        s.appliedBass = -1.f;
    }
    alGetError();
}

F32 LLAudioEngine_OpenAL::streamDryAttn(const StreamSourceParams& p)
{
    // OpenAL's inverse distance clamped with the source's reference / max / rolloff, and its cone
    // (full angles, linear between inner and outer as OpenAL Soft).
    if (p.relative) return 1.f;
    const LLVector3 to_l = (mListenerp ? mListenerp->getPosition() : LLVector3::zero) - p.pos;
    const F32 dist = to_l.length();
    const F32 ref = std::max(p.refDist, 0.001f);
    const F32 d = llclamp(dist, ref, std::max(p.maxDist, ref));
    F32 g = ref / (ref + std::max(p.rolloff, 0.f) * (d - ref));
    if (p.cone && dist > 1e-3f)
    {
        LLVector3 axis = p.dir;
        if (axis.normVec() > 0.f)
        {
            const F32 angle = 2.f * std::acos(llclamp((axis * to_l) / dist, -1.f, 1.f)) * RAD_TO_DEG;
            if (angle > p.coneInner)
            {
                const F32 span = p.coneOuter - p.coneInner;
                const F32 x = span > 1e-3f ? llclamp((angle - p.coneInner) / span, 0.f, 1.f) : 1.f;
                g *= 1.f + (p.coneOuterGain - 1.f) * x;
            }
        }
    }
    return g;
}

void LLAudioEngine_OpenAL::streamWetDecay(const StreamSourceParams& p, const LLAcousticsReverb& r, F32& gain, F32& hf)
{
    // What OpenAL Soft (1.24.3, alu.cpp CalcAttnSourceParams) does with
    // AL_AUXILIARY_SEND_FILTER_GAIN_AUTO to a channel (reference 1 m, rolloff = the listener's,
    // inverse distance clamped with no maximum): beyond the reference,
    //   distance_meters = (d - ref) x rolloff, DryAttnBase = ref / (ref + rolloff (d - ref)),
    //   wet *= 0.001^(distance_meters / (decay x 343.3)) x (1 - DryAttnBase) + DryAttnBase.
    // (The HF of the wet path is not touched by it; air absorption is 0 by default.)
    constexpr F32 kSpeedOfSound = 343.3f;
    constexpr F32 kDecayGain = 0.001f;
    gain = 1.f;
    hf = 1.f;
    const LLVector3 rel = p.relative ? p.pos : p.pos - (mListenerp ? mListenerp->getPosition() : LLVector3::zero);
    const F32 rolloff = mListenerp ? std::max(mListenerp->getRolloffFactor(), 0.f) : 1.f;
    const F32 dist = (rel.length() - 1.f) * rolloff;
    if (!(dist > 0.f)) return;
    const F32 dry_attn = 1.f / (1.f + dist);
    const F32 base = llclamp(r.decay_time, 0.1f, 20.f) * kSpeedOfSound;
    gain = std::pow(kDecayGain, dist / base) * (1.f - dry_attn) + dry_attn;
}

void LLAudioEngine_OpenAL::streamDetachEfx(StreamSrc& s, bool delete_filters)
{
    if (s.directAttached) alSourcei(s.source, AL_DIRECT_FILTER, AL_FILTER_NULL);
    if (s.sendAttached) alSource3i(s.source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
    if (s.zoneSlotAttached) alSource3i(s.source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 1, AL_FILTER_NULL);
    if (s.bassSlotAttached) alSource3i(s.source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 2, AL_FILTER_NULL);
    s.directAttached = s.sendAttached = false;
    s.zoneSlotAttached = 0;
    s.bassSlotAttached = 0;
    s.appliedDirect = s.appliedHF = s.appliedSend = s.appliedSendHF = s.appliedZone = s.appliedZoneHF = -1.f;
    s.appliedBass = -1.f;
    if (delete_filters && p_alDeleteFilters)
    {
        for (ALuint* fp : { &s.filter, &s.sendFilter, &s.zoneFilter, &s.bassFilter })
            if (*fp) { p_alDeleteFilters(1, fp); *fp = 0; }
    }
    alGetError();
}

void LLAudioEngine_OpenAL::detachStreamSends(ALuint only_slot)
{
    for (auto& kv : mStreamSrcs)
    {
        StreamSrc& s = kv.second;
        if (s.zoneSlotAttached && (only_slot == 0 || s.zoneSlotAttached == only_slot))
        {
            alSource3i(s.source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 1, AL_FILTER_NULL);
            s.zoneSlotAttached = 0;
            s.appliedZone = s.appliedZoneHF = -1.f;
        }
        if (only_slot == 0 && s.sendAttached)
        {
            alSource3i(s.source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 0, AL_FILTER_NULL);
            s.sendAttached = false;
            s.appliedSend = s.appliedSendHF = -1.f;
        }
        if (s.bassSlotAttached && (only_slot == 0 || s.bassSlotAttached == only_slot))   // #897
        {
            alSource3i(s.source, AL_AUXILIARY_SEND_FILTER, AL_EFFECTSLOT_NULL, 2, AL_FILTER_NULL);
            s.bassSlotAttached = 0;
            s.appliedBass = -1.f;
        }
    }
}

F64 LLAudioEngine_OpenAL::streamDeviceSeconds()
{
    if (!mDevice) return -1.0;
    streamLoadExt();
    const S64 ns = streamClockNs();
    return ns < 0 ? -1.0 : (F64)ns * 1e-9;
}

LLAudioEngine::StreamStats LLAudioEngine_OpenAL::streamStats() const
{
    StreamStats st = mStreamStats;
    st.open = mStreamOpen;
    st.playing = mStreamPlaying && !mStreamPaused;
    st.paused = mStreamPaused;
    st.syncStart = p_alSourcePlayAtTimeSOFT != nullptr && p_alSourcePlayAtTimevSOFT != nullptr;
    st.latencyExt = p_alGetSourcei64vSOFT != nullptr;
    st.rate = mStreamRate;
    st.sources = (U32)mStreamSrcs.size();
    st.buffersLive = mStreamBuffersLive;
    st.buffersPooled = (U32)mStreamPool.size();
    st.chunks = (U32)mStreamChunks.size();
    st.queuedSeconds = mStreamLastQueued;
    return st;
}

bool LLAudioEngine_OpenAL::initWind()
{
    ALenum error;
    LL_INFOS() << "LLAudioEngine_OpenAL::initWind() start" << LL_ENDL;

    mNumEmptyWindALBuffers = MAX_NUM_WIND_BUFFERS;

    alGetError(); /* clear error */

    alGenSources(1,&mWindSource);

    if((error=alGetError()) != AL_NO_ERROR)
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::initWind() Error creating wind sources: "<<error<<LL_ENDL;
    }

    mWindGen = new LLWindGen<WIND_SAMPLE_T>;

    mWindBufFreq = mWindGen->getInputSamplingRate();
    mWindBufSamples = llceil(mWindBufFreq * WIND_BUFFER_SIZE_SEC);
    mWindBufBytes = mWindBufSamples * 2 /*stereo*/ * sizeof(WIND_SAMPLE_T);

    mWindBuf = new WIND_SAMPLE_T [mWindBufSamples * 2 /*stereo*/];

    if(mWindBuf==NULL)
    {
        LL_ERRS() << "LLAudioEngine_OpenAL::initWind() Error creating wind memory buffer" << LL_ENDL;
        return false;
    }

    LL_INFOS() << "LLAudioEngine_OpenAL::initWind() done" << LL_ENDL;

    return true;
}

void LLAudioEngine_OpenAL::cleanupWind()
{
    LL_INFOS() << "LLAudioEngine_OpenAL::cleanupWind()" << LL_ENDL;

    if (mWindSource != AL_NONE)
    {
        // detach and delete all outstanding buffers on the wind source
        alSourceStop(mWindSource);
        ALint processed;
        alGetSourcei(mWindSource, AL_BUFFERS_PROCESSED, &processed);
        while (processed--)
        {
            ALuint buffer = AL_NONE;
            alSourceUnqueueBuffers(mWindSource, 1, &buffer);
            alDeleteBuffers(1, &buffer);
        }

        // delete the wind source itself
        alDeleteSources(1, &mWindSource);

        mWindSource = AL_NONE;
    }

    delete[] mWindBuf;
    mWindBuf = NULL;

    delete mWindGen;
    mWindGen = NULL;
}

void LLAudioEngine_OpenAL::updateWind(LLVector3 wind_vec, F32 camera_altitude)
{
    LLVector3 wind_pos;
    F64 pitch;
    F64 center_freq;
    ALenum error;
    ALuint *buffers = NULL;

    if (!mEnableWind)
        return;

    if(!mWindBuf)
        return;

    if (mWindUpdateTimer.checkExpirationAndReset(LL_WIND_UPDATE_INTERVAL))
    {

        // wind comes in as Linden coordinate (+X = forward, +Y = left, +Z = up)
        // need to convert this to the conventional orientation DS3D and OpenAL use
        // where +X = right, +Y = up, +Z = backwards
        wind_vec.setVec(-wind_vec.mV[1], wind_vec.mV[2], -wind_vec.mV[0]);

        pitch = 1.0 + mapWindVecToPitch(wind_vec);
        center_freq = 80.0 * pow(pitch,2.5*(mapWindVecToGain(wind_vec)+1.0));

        mWindGen->mTargetFreq = (F32)center_freq;
        mWindGen->mTargetGain = (F32)mapWindVecToGain(wind_vec) * mMaxWindGain;
        mWindGen->mTargetPanGainR = (F32)mapWindVecToPan(wind_vec);

        alSourcei(mWindSource, AL_LOOPING, AL_FALSE);
        alSource3f(mWindSource, AL_POSITION, 0.0, 0.0, 0.0);
        alSource3f(mWindSource, AL_VELOCITY, 0.0, 0.0, 0.0);
        alSourcef(mWindSource, AL_ROLLOFF_FACTOR, 0.0);
        alSourcei(mWindSource, AL_SOURCE_RELATIVE, AL_TRUE);
    }

    // ok lets make a wind buffer now

    ALint processed, queued, unprocessed;
    alGetSourcei(mWindSource, AL_BUFFERS_PROCESSED, &processed);
    alGetSourcei(mWindSource, AL_BUFFERS_QUEUED, &queued);
    unprocessed = queued - processed;

    // ensure that there are always at least 3x as many filled buffers
    // queued as we managed to empty since last time.
    mNumEmptyWindALBuffers = llmin(mNumEmptyWindALBuffers + processed * 3 - unprocessed, MAX_NUM_WIND_BUFFERS-unprocessed);
    mNumEmptyWindALBuffers = llmax(mNumEmptyWindALBuffers, 0);

    //LL_INFOS() << "mNumEmptyWindALBuffers: " << mNumEmptyWindALBuffers    <<" (" << unprocessed << ":" << processed << ")" << LL_ENDL;

    //delete the old wind buffers
    buffers = new ALuint[processed];
    alGetError(); /* clear error */
    alSourceUnqueueBuffers(mWindSource, processed, &buffers[0]);
    error = alGetError();
    if(error != AL_NO_ERROR)
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::updateWind() error swapping (unqueuing) buffers" << LL_ENDL;
    }
    else
    {
        alDeleteBuffers(processed, &buffers[0]);
    }
    // We dont need to keep track of the buffers' id now.
    delete[] buffers;
    buffers = NULL;

    //create the buffers for the empty wind buffers
    unprocessed += mNumEmptyWindALBuffers;
    buffers = new ALuint[mNumEmptyWindALBuffers];
    alGetError(); /* clear error */
    alGenBuffers(mNumEmptyWindALBuffers,&buffers[0]);
    if((error=alGetError()) != AL_NO_ERROR)
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::updateWind() Error creating wind buffer: " << error << LL_ENDL;
        //break;
    }

    //fill the buffers with generated wind.
    int errors = 0;
    for(int i = 0; i < mNumEmptyWindALBuffers; i++)
    {
        alBufferData(buffers[i],
                    AL_FORMAT_STEREO_FLOAT32,
                    mWindGen->windGenerate(mWindBuf,
                            mWindBufSamples),
                    mWindBufBytes,
                    mWindBufFreq);
        error = alGetError();
        if(error != AL_NO_ERROR)
        {
            LL_WARNS() << "LLAudioEngine_OpenAL::updateWind() error swapping (bufferdata) buffers" << LL_ENDL;
            errors++;
        }
    }

    //queue the buffers
    alSourceQueueBuffers(mWindSource, mNumEmptyWindALBuffers, &buffers[0]);
    error = alGetError();
    if(error != AL_NO_ERROR)
    {
        LL_WARNS() << "LLAudioEngine_OpenAL::updateWind() error swapping (queuing) buffers" << LL_ENDL;
    }

    mNumEmptyWindALBuffers = errors;
    // We dont need to keep track of the buffers' id now.
    delete[] buffers;
    buffers = NULL;


    //restart playing if not playing
    ALint playing;
    alGetSourcei(mWindSource, AL_SOURCE_STATE, &playing);
    if(playing != AL_PLAYING)
    {
        alSourcePlay(mWindSource);

        LL_DEBUGS() << "Wind had stopped - probably ran out of buffers - restarting: " << (unprocessed+mNumEmptyWindALBuffers) << " now queued." << LL_ENDL;
    }
}

