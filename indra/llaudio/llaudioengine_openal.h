/**
 * @file audioengine_openal.cpp
 * @brief implementation of audio engine using OpenAL
 * support as a OpenAL 3D implementation
 *
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


#ifndef LL_AUDIOENGINE_OPENAL_H
#define LL_AUDIOENGINE_OPENAL_H

#include "llaudioengine.h"
#include "lllistener_openal.h"
#include "llwindgen.h"

#include <deque>
#include <map>
#include <vector>

class LLAudioEngine_OpenAL : public LLAudioEngine
{
    public:
        LLAudioEngine_OpenAL();
        virtual ~LLAudioEngine_OpenAL();

        virtual bool init(void *user_data, const std::string &app_title);
        virtual std::string getDriverName(bool verbose);
        virtual LLStreamingAudioInterface* createDefaultStreamingAudioImpl() const { return nullptr; }
        virtual void allocateListener();

        virtual void shutdown();

        // Opt-in OpenAL Soft HRTF (set before init; the viewer feeds it from AudioOpenALHRTF).
        static bool sEnableHRTF;

        void setInternalGain(F32 gain);

        LLAudioBuffer* createBuffer();
        LLAudioChannel* createChannel();

        /*virtual*/ bool initWind();
        /*virtual*/ void cleanupWind();
        /*virtual*/ void updateWind(LLVector3 direction, F32 camera_altitude);

        // #683 acoustics (EFX, ALC_EXT_EFX): one EAX-reverb auxiliary slot + a low-pass direct
        // filter per channel, created on enable and deleted on disable / shutdown.
        /*virtual*/ bool setAcousticsEnabled(bool on);
        /*virtual*/ bool acousticsActive() const { return mAcOn; }
        /*virtual*/ void setAcousticsReverb(const LLAcousticsReverb& reverb);
        ALuint acousticsSlot() const { return mAcSlot; }
        // #857 reverb zones: extra EAX-reverb slots on each source's send 1 (needs >= 2 sends).
        /*virtual*/ U32  acousticsZoneCapacity() const;
        /*virtual*/ U32  setAcousticsZoneCount(U32 count);
        /*virtual*/ void setAcousticsZoneReverb(U32 zone, const LLAcousticsReverb& reverb);
        ALuint acousticsZoneSlot(S32 zone) const
        {
            return (mAcOn && zone >= 0 && (U32)zone < mAcZoneCount) ? mAcZones[zone].slot : 0;
        }
        // #897 boundary bass: the EQ slot on each source's send 2 (needs >= 3 sends).
        /*virtual*/ bool setAcousticsBass(bool on);
        ALuint acousticsBassSlot() const { return mAcOn ? mAcBassSlot : 0; }
        // A boost of `db` at low frequencies -> the send level into the EQ slot and the factor on
        // the direct path that cancels the slot's floor leaking into the mids (coherent sum).
        static void bassSend(F32 db, F32& send, F32& direct_mul);

        // #783 local loops: one AL buffer + one relative looping source each.
        /*virtual*/ U32  createLocalLoop(const S16* pcm, U32 frames, U32 channels, U32 rate);
        /*virtual*/ void setLocalLoop(U32 id, F32 gain, F32 pitch);
        /*virtual*/ void destroyLocalLoop(U32 id);
        /*virtual*/ void setLocalLoopOffset(U32 id, const LLVector3& offset);
        /*virtual*/ U32  createLocalLoopShared(U32 base);
        /*virtual*/ void seekLocalLoop(U32 id, U32 frame);
        /*virtual*/ S32  localLoopPosition(U32 id);
        // #783 local shots (thunder): the same records, not looping, positioned, filtered.
        /*virtual*/ U32  createLocalShot(const S16* pcm, U32 frames, U32 channels, U32 rate);
        /*virtual*/ void playLocalShot(U32 id, const LLVector3& offset, F32 gain, F32 pitch);
        /*virtual*/ void setLocalShot(U32 id, const LLVector3& offset, F32 gain, F32 direct_gain, F32 direct_hf, F32 send);
        /*virtual*/ bool localShotPlaying(U32 id);

        // #892 stream speakers (llaudioengine.h).
        /*virtual*/ bool streamOpen(U32 rate);
        /*virtual*/ void streamClose();
        /*virtual*/ bool streamIsOpen() const { return mStreamOpen; }
        /*virtual*/ bool streamPush(const S16* const* pcm, U32 frames);
        /*virtual*/ F64  streamQueuedSeconds();
        /*virtual*/ void streamFlush();
        /*virtual*/ void streamPause(bool pause);
        /*virtual*/ void streamUpdate(F64 prebuffer_secs);
        /*virtual*/ U32  streamCreateSource(EStreamRole role);
        /*virtual*/ void streamSetSource(U32 id, const StreamSourceParams& params);
        /*virtual*/ void streamDestroySource(U32 id);
        /*virtual*/ F64  streamDeviceSeconds();
        /*virtual*/ StreamStats streamStats() const;
        /*virtual*/ void streamSetSyncTolerance(F32 ms) { mStreamSyncTolMs = ms; }

        // #1122 output device (ALC_ENUMERATE_ALL_EXT + ALC_SOFT_reopen_device).
        /*virtual*/ std::vector<std::string> listOutputDevices();
        /*virtual*/ std::vector<std::string> listInputDevices();
        /*virtual*/ std::string defaultOutputDevice();
        /*virtual*/ std::string defaultInputDevice();
        /*virtual*/ std::string currentOutputDevice() const { return mOutputName; }
        /*virtual*/ bool switchOutputDevice(const std::string& name, bool limiter = false);

    private:
        // ---- #892 stream speakers ----
        struct StreamChunk
        {
            U64 seq = 0;
            U64 start = 0;                          // first frame on the stream timeline
            U32 frames = 0;
            std::vector<S16> pcm[STREAM_ROLE_COUNT];   // kept until played (late joiners need it)
            ALuint buf[STREAM_ROLE_COUNT] = {};     // made on first use per role
            U32 refs[STREAM_ROLE_COUNT] = {};       // sources holding buf[role] in their AL queue
        };
        struct StreamSrc
        {
            ALuint source = 0;
            EStreamRole role = STREAM_MONO;
            enum EState : U8 { WAITING, ACTIVE, JOINING } state = WAITING;
                                                    // WAITING: queued from the front, starts with the group
                                                    // ACTIVE: playing with the group
                                                    // JOINING: nothing queued yet, joins at a boundary
            U64 head = 0;                           // seq of the oldest buffer in its AL queue
            U64 tail = 0;                           // seq of the next chunk to queue (head == tail: empty)
            S64 startAtNs = 0;                      // PlayAtTime start (device ns), 0 = none / started
            StreamSourceParams applied;
            bool appliedValid = false;
            ALuint filter = 0, sendFilter = 0, zoneFilter = 0;
            bool directAttached = false, sendAttached = false;
            ALuint zoneSlotAttached = 0;
            F32 appliedDirect = -1.f, appliedHF = -1.f, appliedSend = -1.f, appliedSendHF = -1.f;
            F32 appliedZone = -1.f, appliedZoneHF = -1.f;
            ALuint bassFilter = 0;                  // #897 send 2 -> the boundary-bass EQ slot
            ALuint bassSlotAttached = 0;
            F32 appliedBass = -1.f;
            bool joinChecked = true;                // the sync guard has measured it since its last join
            bool headRel = false;                   // AL_SOURCE_RELATIVE as applied
            S64  lastResyncNs = 0;                  // device time of its last guard resync
        };
        bool   streamLoadExt();
        S64    streamClockNs();                             // the device clock (ns), -1 = none
        bool   streamRunning(const StreamSrc& s, S64 now_ns);   // playing, or a scheduled start still ahead
        StreamChunk* streamChunk(U64 seq);
        bool   streamQueueOn(StreamSrc& s, U64 upto);      // queue chunks [s.tail, upto) on s
        void   streamUnqueueProcessed(StreamSrc& s);
        void   streamUnqueueAll(StreamSrc& s);              // (stopped source) everything back
        void   streamDropPlayed();                          // front chunks nobody holds and everyone passed
        void   streamReleaseChunk(StreamChunk& c);
        bool   streamPlayhead(U64& pos_frames, S64* clock_ns, F64* frac);   // a running source's position
        void   streamRestartAll(U64 resume_seq, bool underrun);
        void   streamStartGroup();
        enum EJoin { JOIN_DONE, JOIN_WAIT, JOIN_UNSUPPORTED };
        EJoin  streamJoin(StreamSrc& s);
        U64    streamBoundaryAtOrAfter(U64 pos_frames);     // seq of the first chunk starting at / after pos
        void   streamApplyEfx(StreamSrc& s, const StreamSourceParams& p);
        void   streamSyncGuard();                           // ~1 Hz: measure / resync (streamUpdate)
        LLVector3 toListenerFrame(const LLVector3& v, bool point);   // OpenAL's listener matrix (no-doppler speakers)
        // The wet path's distance decay a channel gets from OpenAL (see streamCreateSource).
        void   streamWetDecay(const StreamSourceParams& p, const LLAcousticsReverb& r, F32& gain, F32& hf);
        void   streamDetachEfx(StreamSrc& s, bool delete_filters);
        void   detachStreamSends(ALuint only_slot);         // 0: every send (room + zones + bass)
        // The distance / cone attenuation OpenAL gives a speaker's dry path (#897: its bass send gets none).
        F32    streamDryAttn(const StreamSourceParams& p);

        bool   mStreamOpen = false;
        bool   mStreamExtTried = false;
        U32    mStreamRate = 0;
        bool   mStreamPlaying = false;
        bool   mStreamPaused = false;
        U64    mStreamNextSeq = 0;
        U64    mStreamEnd = 0;                              // frames pushed (timeline end)
        std::deque<StreamChunk> mStreamChunks;
        std::map<U32, StreamSrc> mStreamSrcs;
        U32    mStreamNextId = 1;
        std::vector<ALuint> mStreamPool;                    // free AL buffers (bounded)
        U32    mStreamBuffersLive = 0;
        S64    mStreamStartAtNs = 0;                        // device time the group (re)starts at (joins wait for it)
        F32    mStreamSyncTolMs = 10.f;
        S64    mStreamSyncNextNs = 0;
        U32    mStreamSyncWinChecks = 0;
        F32    mStreamSyncWinOffset = 0.f, mStreamSyncWinSpread = 0.f, mStreamSyncWinJoin = 0.f;
        bool   mStreamRestartNow = false;                   // a desync restart: start without the full pre-buffer
        F64    mStreamLastQueued = 0.0;
        StreamStats mStreamStats;

        struct LocalLoop
        {
            ALuint buffer = 0;
            ALuint source = 0;
            F32    gain = -1.f;     // last applied (-1: none yet)
            F32    pitch = -1.f;
            bool   playing = false;
            bool   ownsBuffer = true;   // #1012: false for a source sharing another loop's buffer
            U32    frames = 0;
            bool   placed = false;  // #894: a loop placed in the world (setLocalLoopOffset), else listener-relative
            // Shots only (#783 thunder):
            bool   shot = false;
            ALuint filter = 0;      // direct low-pass (EFX)
            ALuint sendFilter = 0;  // room send level (EFX, while acoustics is on)
            bool   sendAttached = false;
            F32    appliedHF = -1.f, appliedDirect = -1.f, appliedSend = -1.f;
        };
        std::map<U32, LocalLoop> mLocalLoops;
        U32 mNextLocalLoop = 1;
        void destroyLocalLoops();
        U32  createLocalSource(const S16* pcm, U32 frames, U32 channels, U32 rate, bool loop);
        bool efxReady();             // the EFX entry points, loaded once (acoustics on or not)
        void detachShotSends();      // before the room slot goes away

        bool efxLoad();              // ALC_EXT_EFX + aux sends + entry points (once per engine)
        void acousticsTeardown();    // detach every channel, delete the slot / effect
        void writeReverb(ALuint effect, const LLAcousticsReverb& r);
        // One reverb slot's update (the room's and the zones'): wet = slot gain, the effect
        // reloaded only on a real change.
        void applyReverb(ALuint slot, ALuint effect, const LLAcousticsReverb& r, LLAcousticsReverb& applied,
                         bool& applied_valid, F32& wet_applied);
        void deleteZones(U32 from);  // zones >= from: channels' send 1 detached, slot + effect deleted
        void deleteBass();           // #897: channels' / speakers' send 2 detached, slot + effect deleted

        bool   mAcOn = false;
        bool   mEfxTried = false;
        bool   mEfxOk = false;
        bool   mAcEax = false;       // AL_EFFECT_EAXREVERB (else AL_EFFECT_REVERB)
        ALuint mAcSlot = 0;
        ALuint mAcEffect = 0;
        bool   mAcAppliedValid = false;
        LLAcousticsReverb mAcApplied;
        F32    mAcWetApplied = -1.f;
        ALCint mAcSends = 0;         // ALC_MAX_AUXILIARY_SENDS of this context (efxLoad)
        struct AcZone
        {
            ALuint slot = 0;
            ALuint effect = 0;
            bool   applied_valid = false;
            LLAcousticsReverb applied;
            F32    wet_applied = -1.f;
        };
        AcZone mAcZones[kAcousticsMaxZones];
        U32    mAcZoneCount = 0;
        ALuint mAcBassSlot = 0;      // #897 boundary bass (EQ)
        ALuint mAcBassEffect = 0;
        bool   mAcBassWarned = false;

        typedef F32 WIND_SAMPLE_T;
        LLWindGen<WIND_SAMPLE_T> *mWindGen;
        F32 *mWindBuf;
        U32 mWindBufFreq;
        U32 mWindBufSamples;
        U32 mWindBufBytes;
        ALuint mWindSource;
        int mNumEmptyWindALBuffers;

        static const int MAX_NUM_WIND_BUFFERS = 80;
        static const float WIND_BUFFER_SIZE_SEC; // 1/20th sec

        ALCdevice*  mDevice = nullptr;
        ALCcontext* mContext = nullptr;
        std::vector<ALCint> mDeviceAttrs;   // the context attributes init chose (reused by a reopen)
        std::string mOutputName;            // #1122: "" = the system default (the device opened with NULL)
        bool mOutputLimiter = false;        // #1122: the last reopen asked for ALC_OUTPUT_LIMITER_SOFT
};

class LLAudioChannelOpenAL : public LLAudioChannel
{
    public:
        LLAudioChannelOpenAL();
        virtual ~LLAudioChannelOpenAL();
    protected:
        /*virtual*/ void play();
        /*virtual*/ void playSynced(LLAudioChannel *channelp);
        /*virtual*/ void cleanup();
        /*virtual*/ bool isPlaying();

        /*virtual*/ bool updateBuffer();
        /*virtual*/ void update3DPosition();
        F32 distanceGain() const;   // LLAudioEngine::sDistanceGain (AudioInverseDistanceRolloff), 1 = none
        /*virtual*/ void updateLoop();

        // #683 acoustics: filter + room send while the engine's acoustics is active and the
        // source is eligible; detached (AL_FILTER_NULL / AL_EFFECTSLOT_NULL) otherwise.
        void applyAcoustics();
        void detachAcoustics(bool delete_filter);
        void detachZoneSend();       // #857: send 1 -> AL_EFFECTSLOT_NULL
        void detachBassSend();       // #897: send 2 -> AL_EFFECTSLOT_NULL
        F32  dryAttenuation() const; // #897: OpenAL's distance attenuation of the dry path
        F32  acousticsBoost() const; // #683 early gain: this source's level boost (1 = none / acoustics off)
        void applySourceGain();      // AL_GAIN (x the boost) and AL_MAX_GAIN
        friend class LLAudioEngine_OpenAL;

        ALuint mALSource;
            ALint mLastSamplePos;
        ALuint mALFilter = 0;
        ALuint mALSendFilter = 0;   // low-pass on the aux send (room reverb)
        bool   mAcAttached = false;
        F32    mAcAppliedGain = -1.f;
        F32    mAcAppliedGainHF = -1.f;
        F32    mAcAppliedSendGain = -1.f;
        F32    mAcAppliedSendGainHF = -1.f;
        ALuint mALZoneFilter = 0;       // #857 low-pass on send 1 (reverb zone)
        ALuint mAcZoneSlotAttached = 0; // the zone slot send 1 points at (0 = none)
        F32    mAcAppliedZoneGain = -1.f;
        F32    mAcAppliedZoneGainHF = -1.f;
        ALuint mALBassFilter = 0;       // #897 low-pass (HF off) on send 2 (boundary bass)
        ALuint mAcBassAttached = 0;     // the bass slot send 2 points at (0 = none)
        F32    mAcAppliedBass = -1.f;
        F32    mAcAppliedMaxGain = 1.f;   // #683 early gain: AL_MAX_GAIN as set (1 = OpenAL's default)
};

class LLAudioBufferOpenAL : public LLAudioBuffer{
    public:
        LLAudioBufferOpenAL();
        virtual ~LLAudioBufferOpenAL();

        bool loadWAV(const std::string& filename);
        U32 getLength();

        friend class LLAudioChannelOpenAL;
    protected:
        void cleanup();
        ALuint getBuffer() {return mALBuffer;}

        ALuint mALBuffer;
};

#endif
