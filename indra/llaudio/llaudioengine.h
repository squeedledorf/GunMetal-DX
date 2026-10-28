/**
 * @file audioengine.h
 * @brief Definition of LLAudioEngine base class abstracting the audio support
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
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


#ifndef LL_AUDIOENGINE_H
#define LL_AUDIOENGINE_H

#include <list>
#include <map>
#include <array>
#include <vector>

#include "v3math.h"
#include "v3dmath.h"
#include "lltimer.h"
#include <functional>
#include "lluuid.h"
#include "llframetimer.h"
#include "llassettype.h"
#include "llextendedstatus.h"

#include "lllistener.h"
#include "boost/unordered_map.hpp"

const F32 LL_WIND_UPDATE_INTERVAL = 0.1f;
const F32 LL_WIND_UNDERWATER_CENTER_FREQ = 20.f;

const F32 ATTACHED_OBJECT_TIMEOUT = 5.0f;
const F32 DEFAULT_MIN_DISTANCE = 2.0f;

#define LL_MAX_AUDIO_CHANNELS 30
// 40 thrashed in busy sims: with more distinct ambient sounds than buffers, idle
// sources stole the oldest buffer and re-read its WAV from disk on the main thread
// every frame (~0.5 ms/frame). Buffers are just decoded PCM (<= ~0.9 MB each).
#define LL_MAX_AUDIO_BUFFERS 128

class LLAudioSource;
class LLAudioData;
class LLAudioChannel;
class LLAudioChannelOpenAL;
class LLAudioBuffer;
class LLStreamingAudioInterface;
struct SoundData;

// ACOUSTICS (#683 phase 0, AudioAcoustics): ONE listener-room reverb on an EFX auxiliary
// effect slot every in-world sound sends to (EAX reverb; plain EFX reverb takes the subset).
// Values are EFX units (AL_EAXREVERB_*); defaults = the EFX generic preset, wet 0 = dry.
struct LLAcousticsReverb
{
    F32 wet = 0.f;                   // AL_EFFECTSLOT_GAIN (0..1)
    F32 decay_time = 1.49f;          // s
    F32 decay_hf_ratio = 0.83f;
    F32 density = 1.f;
    F32 diffusion = 1.f;
    F32 gain = 0.32f;
    F32 gain_hf = 0.89f;
    F32 reflections_gain = 0.05f;
    F32 reflections_delay = 0.007f;  // s
    F32 late_gain = 1.26f;
    F32 late_delay = 0.011f;         // s
    // #683 phase 1 (measured by the bounce pass; EAX reverb only, the standard reverb ignores them).
    F32 decay_lf_ratio = 1.f;        // AL_EAXREVERB_DECAY_LFRATIO (LF reference 250 Hz)
    F32 gain_lf = 1.f;               // AL_EAXREVERB_GAINLF (<= 1)
    // Pan vectors in the LISTENER's frame, EAX convention (left-handed: x right, y up, z FORWARD);
    // direction = where the reflections / tail come from, length = focus (0 all around .. 1).
    F32 reflections_pan[3] = {0.f, 0.f, 0.f};
    F32 late_pan[3] = {0.f, 0.f, 0.f};
};

//
//  LLAudioEngine definition
//
class LLAudioEngine
{
    friend class LLAudioChannelOpenAL; // bleh. channel needs some listener methods.

public:
    enum LLAudioType
    {
        AUDIO_TYPE_NONE    = 0,
        AUDIO_TYPE_SFX     = 1,
        AUDIO_TYPE_UI      = 2,
        AUDIO_TYPE_AMBIENT = 3,
        AUDIO_TYPE_COUNT   = 4 // last
    };

    enum LLAudioPlayState
    {
        // isInternetStreamPlaying() returns an *S32*, with
        // 0 = stopped, 1 = playing, 2 = paused.
        AUDIO_STOPPED = 0,
        AUDIO_PLAYING = 1,
        AUDIO_PAUSED = 2
    };

    LLAudioEngine();
    virtual ~LLAudioEngine();

    // initialization/startup/shutdown
    virtual bool init(void *userdata, const std::string &app_title);
    virtual std::string getDriverName(bool verbose) = 0;
    virtual LLStreamingAudioInterface *createDefaultStreamingAudioImpl() const = 0;
    virtual void shutdown();

    // Used by the mechanics of the engine
    //virtual void processQueue(const LLUUID &sound_guid);
    virtual void setListener(LLVector3 pos,LLVector3 vel,LLVector3 up,LLVector3 at);
    virtual void updateWind(LLVector3 direction, F32 camera_height_above_water) = 0;
    virtual void idle();
    virtual void updateChannels();

    //
    // "End user" functionality
    //
    virtual bool isWindEnabled();
    virtual void enableWind(bool state_b);

    // Use these for temporarily muting the audio system.
    // Does not change buffers, initialization, etc. but
    // stops playing new sounds.
    void setMuted(bool muted);
    bool getMuted() const { return mMuted; }
#ifdef USE_PLUGIN_MEDIA
    LLPluginClassMedia* initializeMedia(const std::string& media_type);
#endif
    F32 getMasterGain();
    void setMasterGain(F32 gain);
    // #1122: a linear gain on the whole engine output on top of the master volume (the VR
    // headset gain). 1 = none (the default; nothing else ever sets it).
    void setOutputBoost(F32 boost);
    F32 getOutputBoost() const { return mOutputBoost; }

    F32 getSecondaryGain(S32 type);
    void setSecondaryGain(S32 type, F32 gain);

    F32 getInternetStreamGain();

    virtual void setDopplerFactor(F32 factor);
    virtual F32 getDopplerFactor();
    virtual void setRolloffFactor(F32 factor);
    virtual F32 getRolloffFactor();
    virtual void setMaxWindGain(F32 gain);

    // ---- ACOUSTICS (#683 phase 0; the newview layer drives it: llcleanacoustics.h) ----
    // Engines without EFX keep these no-ops (the feature is inert). Off = no EFX object
    // exists and no source has a filter or send: exactly the plain path.
    // Turn the EFX path on / off; returns whether it is active afterwards.
    virtual bool setAcousticsEnabled(bool /*on*/) { return false; }
    virtual bool acousticsActive() const { return false; }
    // The room reverb (smoothed by the caller; the engine reloads the effect only on change).
    virtual void setAcousticsReverb(const LLAcousticsReverb& /*reverb*/) {}
    // #857 REVERB ZONES: up to kAcousticsMaxZones extra reverb slots for sounds in OTHER rooms,
    // reached through a source's SECOND auxiliary send (send 0 stays the listener's room).
    // Capacity 0 = no second send on this device (the feature then stays off). Count changes
    // create / delete slots (never per frame; 0 = no slot exists); a zone's wet is its slot gain.
    static constexpr U32 kAcousticsMaxZones = 3;
    virtual U32  acousticsZoneCapacity() const { return 0; }
    virtual U32  setAcousticsZoneCount(U32 /*count*/) { return 0; }   // returns the count now available
    virtual void setAcousticsZoneReverb(U32 /*zone*/, const LLAcousticsReverb& /*reverb*/) {}
    // #897 BOUNDARY BASS: one EQUALIZER slot (a low shelf at kAcousticsBassShelfHz boosted
    // +12 dB, everything above at its -18 dB floor) every in-world source may send to on its THIRD
    // auxiliary send. Sent in parallel to the dry path it adds low end: the engine turns a
    // requested boost in dB into the send level (accounting for the shelf's floor leaking into
    // the mids, and for the distance attenuation OpenAL gives the dry path but not a non-reverb
    // send). Needs 3 sends (else unavailable: false); off = no slot exists.
    static constexpr F32 kAcousticsBassShelfHz = 180.f;
    static constexpr F32 kAcousticsBassMaxDb = 12.f;
    // #683 EARLY GAIN: the most a source's level may be RAISED by the reflected energy reaching
    // the listener (LLAudioChannel::setAcousticsBoost; the driver's AudioAcousticsEarlyGainMaxDb
    // stays below it): +12 dB.
    static constexpr F32 kAcousticsMaxBoost = 3.981f;
    virtual bool setAcousticsBass(bool /*on*/) { return false; }
    // Channel access for the per-source occlusion filter (LLAudioChannel::setAcousticsDirect).
    LLAudioChannel* getChannelAt(S32 i) const { return (i >= 0 && i < LL_MAX_AUDIO_CHANNELS) ? mChannels[i] : nullptr; }
    // In-world sounds get the room send + occlusion filter; UI / preview (forced priority)
    // sounds never do. (Voice, wind and the parcel stream's stereo bed are not channel sources:
    // always dry. The parcel stream through in-world speakers -- the STREAM SPEAKERS below --
    // gets the same filters and sends from the newview driver: streamSetSource.)
    static bool acousticsEligible(const LLAudioSource* src);
    // The newview layer's extra "keep dry" rule (HUD attachments: llaudio can't see objects).
    static inline std::function<bool(const LLAudioSource*)> sAcousticsKeepDry;

    // Real inverse distance (#892): channels already fall off as OpenAL's inverse distance clamped
    // with reference 1 m and rolloff 1 (x5 under water), i.e. ref / d, -6 dB per doubling, with no
    // maximum distance. A radius is a soft maximum: full inverse distance up to (1 - kSoftEdge) x r,
    // then a smooth fade to silence at r.
    static constexpr F32 kSoftEdge = 0.15f;
    static F32 softEdge(F32 dist, F32 radius)
    {
        if (!(radius > 0.f)) return 1.f;
        const F32 start = radius * (1.f - kSoftEdge);
        if (dist <= start) return 1.f;
        if (dist >= radius) return 0.f;
        const F32 x = (radius - dist) / (radius - start);   // 1 at the fade's start .. 0 at r
        return x * x * (3.f - 2.f * x);                     // smoothstep
    }
    // The newview layer's extra per-source distance gain for in-world channel sounds (applied on
    // top of OpenAL's falloff; 1 = none). AudioInverseDistanceRolloff: a sound with a script
    // cutoff radius (llSetSoundRadius) fades into it over its last 15 % instead of being cut off
    // there. Unset or the setting off: 1, exactly as before.
    static inline std::function<F32(const LLAudioSource*)> sDistanceGain;

    // ---- LOCAL LOOPS (#783 weather ambience; the newview layer drives them: slweatheraudio.h) ----
    // In-process 16-bit PCM (mono / stereo, interleaved) played as a looping, listener-relative,
    // non-positional source: no asset, no cache, no channel (never competes with in-world
    // voices). The master gain and mute apply (the listener gain); the caller applies its own
    // category gain. A loop is paused while its gain is 0. Engines without support return 0 (the
    // feature stays silent). Handles belong to the engine that made them; shutdown() frees them.
    virtual U32  createLocalLoop(const S16* /*pcm*/, U32 /*frames*/, U32 /*channels*/, U32 /*rate*/) { return 0; }
    virtual void setLocalLoop(U32 /*id*/, F32 /*gain*/, F32 /*pitch*/) {}
    virtual void destroyLocalLoop(U32 /*id*/) {}
    // #894: PLACE a loop -- offset in the listener's world axes (metres; the engine adds the
    // listener position every call, so call it each update while placed), distance attenuation
    // stays off (only the direction is heard; a mono loop pans, a stereo one plays as is). A zero
    // offset puts it back at the listener (listener-relative, non-positional).
    virtual void setLocalLoopOffset(U32 /*id*/, const LLVector3& /*offset*/) {}
    // #1012: a SECOND source over the same buffer as loop `base` (no copy of the PCM; own gain,
    // pitch and placement; starts silent). Returns 0 if `base` is not a loop. Destroying `base`
    // destroys the sources sharing its buffer too. localLoopPosition: the playhead in frames
    // (-1 unknown); seekLocalLoop moves it (a paused loop keeps the new point until resumed).
    virtual U32  createLocalLoopShared(U32 /*base*/) { return 0; }
    virtual void seekLocalLoop(U32 /*id*/, U32 /*frame*/) {}
    virtual S32  localLoopPosition(U32 /*id*/) { return -1; }

    // ---- LOCAL SHOTS (#783 thunder; slweatheraudio.h) ----
    // In-process 16-bit PCM kept as one buffer + one source, played once on demand (not
    // looping) at a POSITION relative to the listener (offset in the listener's world axes,
    // metres: the engine adds the listener position every set call), distance attenuation off
    // (the level is baked into the recording: the caller places the sound only for its
    // direction). Mono gives the direction; stereo plays as is. The master gain and mute apply.
    // EFX (ALC_EXT_EFX present): a low-pass on the direct path (gain, gain_hf) and, while the
    // acoustics room reverb is on (setAcousticsEnabled), a send to it (send 0 = dry). Without
    // EFX the filter folds into the gain and the send is dropped. Engines without support return
    // 0. destroyLocalLoop frees a shot too (stopping it); shutdown() frees them all.
    virtual U32  createLocalShot(const S16* /*pcm*/, U32 /*frames*/, U32 /*channels*/, U32 /*rate*/) { return 0; }
    // (Re)start from the beginning. Call setLocalShot first: it sets the level and filters the
    // start plays with (the gain here only applies when no setLocalShot came before).
    virtual void playLocalShot(U32 /*id*/, const LLVector3& /*offset*/, F32 /*gain*/, F32 /*pitch*/) {}
    // While it plays: follow the listener, gain and filtering.
    virtual void setLocalShot(U32 /*id*/, const LLVector3& /*offset*/, F32 /*gain*/, F32 /*direct_gain*/,
                              F32 /*direct_hf*/, F32 /*send*/) {}
    virtual bool localShotPlaying(U32 /*id*/) { return false; }

    // ---- STREAM SPEAKERS (#892 parcel audio; the newview layer drives them: slparcelspeakeraudio.h) ----
    // ONE stream timeline (the parcel stream's captured PCM, pushed in chunks) played by any
    // number of engine-owned streaming sources -- never the 30 channels. Each chunk carries the
    // PCM of every ROLE once (stereo interleaved, the others mono); an AL buffer per role is made
    // on first use and the SAME buffer is queued on every source of that role, so all sources
    // play sample-locked. All sources start together (alSourcePlayAtTimevSOFT, else one
    // alSourcePlayv); a source added while playing joins at the next chunk boundary at the exact
    // device time (alSourcePlayAtTimeSOFT; without it: all restart together). If any source
    // stops (an underrun or a desync) every source is stopped and restarted together.
    // The master gain and mute apply (the listener gain); the caller applies the music level.
    enum EStreamRole : U8
    {
        STREAM_STEREO = 0,  // interleaved L/R (the listener-relative bed: direct channels, no spatialisation)
        STREAM_MONO,        // (L+R)/2
        STREAM_LEFT,
        STREAM_RIGHT,
        STREAM_SUB,         // low-passed mono
        STREAM_ROLE_COUNT
    };
    struct StreamSourceParams
    {
        bool      relative = false;     // true: at the listener, no distance (the bed); pos ignored
        LLVector3 pos;                  // OpenAL space (global position as float, like the channels)
        F32       gain = 0.f;           // music level x speaker level (the listener gain does the rest)
        F32       refDist = 1.f, maxDist = 30.f, rolloff = 1.f;   // inverse distance clamped
        bool      cone = false;
        LLVector3 dir;                  // cone axis, unit, OpenAL space
        F32       coneInner = 360.f, coneOuter = 360.f, coneOuterGain = 0.f;   // full angles, degrees
        // Acoustics (#683, llcleanacoustics.h sourceAcoustics): false = no filter and no send at all.
        bool      efx = false;
        F32       directGain = 1.f, directHF = 1.f;   // occlusion low-pass on the direct path
        F32       sendGain = 0.f, sendHF = 1.f;       // send 0 -> the listener-room reverb
        S32       zone = -1;                          // send 1 -> this reverb zone (#857), < 0 none
        F32       zoneGain = 0.f, zoneHF = 1.f;
        F32       bassBoostDb = 0.f;                  // #897 boundary bass (send 2), 0 = none
        F32       directBoost = 1.f;                  // #683 early gain: level boost (>= 1) on the source gain; the sends are divided by it
        // Diffuse-field reverb (ParcelAudioDiffuseReverb): the sends carry no distance decay -- a
        // reverberant room's tail is about as loud everywhere in it, so a far speaker keeps its
        // reverb. Its level: gain (vol); liveliness: the room slot's wet / decay (and the zone's);
        // coupling: sendGain / zoneGain. false: the channels' rule, the reverb's initial decay
        // over the distance (OpenAL's AL_AUXILIARY_SEND_FILTER_GAIN_AUTO; streamWetDecay).
        bool      diffuse = false;
        // Doppler (ParcelAudioSpeakerDoppler). false: no pitch shift from listener or source
        // motion -- the source is placed head-relative (listener frame, recomputed every call)
        // with velocity 0; OpenAL Soft adds the listener velocity to a head-relative source's,
        // so the doppler factor is exactly 1. Position, falloff, cone and sends are unchanged.
        bool      doppler = true;
    };
    struct StreamStats
    {
        bool open = false;
        bool playing = false;           // the sources are running (else pre-buffering / paused)
        bool paused = false;
        bool syncStart = false;         // alSourcePlayAtTime(v)SOFT available
        bool latencyExt = false;        // AL_SOFT_source_latency (sample offset + device clock)
        U32  rate = 0;
        U32  sources = 0, sourcesPeak = 0;
        U32  buffersLive = 0, buffersPeak = 0, buffersPooled = 0;
        U32  chunks = 0;
        F64  queuedSeconds = 0.0;
        U64  starts = 0;                // group starts (first start, after an underrun, after a restart)
        U64  underruns = 0;             // every source ran dry: pre-buffer again
        U64  restarts = 0;              // a source stopped while others played: all restarted together
        U64  lateJoins = 0;             // sources joined a running group sample-exact
        U64  joinFallbacks = 0;         // joins done by restarting the group (no PlayAtTime)
        U64  flushes = 0;
        U64  pushRejected = 0;          // chunks refused: the queue was at its cap
        // Sync guard (~1 Hz): every playing speaker's timeline position against the stereo bed
        // (head-relative, so never doppler-shifted). Window values: the last ~10 checks.
        F32  syncOffsetMaxMs = 0.f;     // the largest |speaker - bed| offset
        F32  syncSpreadMaxMs = 0.f;     // the largest offset between two speakers
        F32  syncJoinErrMaxMs = 0.f;    // the largest offset a source had at its first check after a join
        U64  syncChecks = 0, syncResyncs = 0, syncDeferred = 0;
    };
    // Open the timeline at `rate` Hz (false: unsupported / failed). Opening an open one is a no-op.
    virtual bool streamOpen(U32 /*rate*/) { return false; }
    // Destroy every stream source and buffer.
    virtual void streamClose() {}
    virtual bool streamIsOpen() const { return false; }
    // Append one chunk: pcm[role] = `frames` frames of that role (STREAM_STEREO interleaved stereo,
    // the others mono), null = silence. false: refused (not open, or kStreamMaxQueueSeconds queued).
    static constexpr F64 kStreamMaxQueueSeconds = 3.0;
    virtual bool streamPush(const S16* const* /*pcm*/, U32 /*frames*/) { return false; }
    // Pushed but not yet heard, seconds.
    virtual F64  streamQueuedSeconds() { return 0.0; }
    // Discontinuity (new stream, seek): stop every source and drop everything queued.
    virtual void streamFlush() {}
    // The producer paused / resumed: pause / resume every source together.
    virtual void streamPause(bool /*pause*/) {}
    // Once per tick after the pushes: reclaim played buffers, queue new chunks, detect underruns /
    // desyncs (restart all), start the group once `prebuffer_secs` are queued, join new sources.
    virtual void streamUpdate(F64 /*prebuffer_secs*/) {}
    // Sync guard tolerance (ms): a speaker further than this from the timeline is re-joined at the
    // next chunk boundary (ParcelAudioSyncToleranceMs). <= 0: the guard only measures.
    virtual void streamSetSyncTolerance(F32 /*ms*/) {}
    // Sources: 0 = failed. A new source plays nothing until the group starts / it has joined.
    virtual U32  streamCreateSource(EStreamRole /*role*/) { return 0; }
    virtual void streamSetSource(U32 /*id*/, const StreamSourceParams& /*params*/) {}
    virtual void streamDestroySource(U32 /*id*/) {}
    // The output device's clock in seconds (ALC_SOFT_device_clock), -1 = not available.
    virtual F64  streamDeviceSeconds() { return -1.0; }
    virtual StreamStats streamStats() const { return StreamStats(); }

    // #1122 output device (VR headset audio, and the user's output device choice in Sound
    // preferences). Never called on the desktop unless the user chose a device or a VR session
    // runs. listOutputDevices / listInputDevices: the backend's device names (enumerated on each
    // call). defaultOutputDevice: the name the system default resolves to ("" = unknown).
    // currentOutputDevice: the device the engine plays on ("" = it follows the system default).
    // switchOutputDevice: move the open engine to `name` ("" = back to the system default)
    // keeping every playing sound; `limiter` asks for the backend's output limiter (used while
    // the output is boosted above unity gain); false = not possible (nothing changed).
    virtual std::vector<std::string> listOutputDevices() { return {}; }
    virtual std::vector<std::string> listInputDevices() { return {}; }
    virtual std::string defaultOutputDevice() { return std::string(); }
    // The name the system default capture device resolves to ("" = unknown): only used to see
    // whether the default mic already is the headset's.
    virtual std::string defaultInputDevice() { return std::string(); }
    virtual std::string currentOutputDevice() const { return std::string(); }
    virtual bool switchOutputDevice(const std::string& /*name*/, bool /*limiter*/ = false) { return false; }


    // Methods actually related to setting up and removing sounds
    // Owner ID is the owner of the object making the request
    // Returns the id of the created audio source (null if the sound was dropped: muted, silent
    // or blocked), so callers can later ask whether it is still playing.
    LLUUID triggerSound(const LLUUID &sound_id, const LLUUID& owner_id, const F32 gain,
                      const S32 type = LLAudioEngine::AUDIO_TYPE_NONE,
                      const LLVector3d &pos_global = LLVector3d::zero);
    void triggerSound(SoundData& soundData);

    // #654 asset-ID sound blocking. The newview layer installs a predicate here (llaudio can't
    // link against newview). Every playback path consults it: triggerSound, LLAudioSource::play,
    // LLAudioSource::addAudioData (preloads/queues) and preloadSound, so a blocked sound is
    // dropped before it is fetched, decoded or played, whoever asks for it.
    static inline std::function<bool(const LLUUID&)> sIsSoundBlocked;
    static bool isSoundBlocked(const LLUUID& id) { return sIsSoundBlocked && id.notNull() && sIsSoundBlocked(id); }
    // Silence every live source currently playing/queueing this asset (called when newly blocked).
    void stopSourcesPlaying(const LLUUID& audio_id);

    bool preloadSound(const LLUUID &id);

    void addAudioSource(LLAudioSource *asp);
    void cleanupAudioSource(LLAudioSource *asp);

    LLAudioSource *findAudioSource(const LLUUID &source_id);
    // Finds or creates the data for a sound; NULL while the sound is marked bad.
    LLAudioData *getAudioData(const LLUUID &audio_uuid);
    // Finds only (never creates, ignores the bad mark).
    LLAudioData *findAudioData(const LLUUID &audio_uuid) const;
    // #883 MemTrend: sounds known (LLAudioData; #891: freed when no source, buffer, fetch
    // or decode uses them -- see sweepAudioData), sources alive, buffers holding decoded
    // PCM, sounds currently marked bad.
    size_t getAudioDataCount() const { return mAllData.size(); }
    size_t getAudioDataPeak() const { return mAudioDataPeak; }
    static constexpr size_t AUDIO_DATA_SOFT_CAP = 4096;
    size_t getSourceCount() const { return mAllSources.size(); }
    size_t getBufferCount() const;
    size_t getCorruptCount() const;

    // #891: why a requested sound did not play. Counted every time; logged once per sound
    // (per reason) while its LLAudioData lives.
    enum ESoundIssue
    {
        ISSUE_FETCH_FAILED = 0, // asset fetch failed or timed out (retried with backoff)
        ISSUE_DECODE_FAILED,    // vorbis decode / decoded-file write failed (refetched, retried)
        ISSUE_MARKED_BAD,       // out of retries: silent until the bad mark expires
        ISSUE_NO_CHANNEL,       // ready, but every channel held a higher-priority sound
        ISSUE_NOT_READY,        // fetch/decode did not finish before the sound was given up
        ISSUE_MUTED,            // muted, blocked, out of cutoff radius or parcel audio
        ISSUE_SUPERSEDED,       // replaced or stopped before it started
        ISSUE_QUEUE_FULL,       // queueing on and the one queue slot was taken (SL semantics)
        ISSUE_QUEUED_BAD,       // the queued clip could not be played; skipped
        ISSUE_COUNT
    };
    static const char* soundIssueName(ESoundIssue issue);
    void noteSoundIssue(LLAudioData* adp, const LLUUID& id, ESoundIssue issue, const std::string& detail = std::string());
    U64 getSoundIssueCount(ESoundIssue issue) const { return issue < ISSUE_COUNT ? mIssueCounts[issue] : 0; }

    // #891 bounded retries: a failed fetch or decode is retried after a backoff, and only
    // after MAX_SOUND_ATTEMPTS failures is the sound marked bad -- for BAD_SOUND_TTL
    // seconds, not the session. Decode failures drop the cached asset so the retry
    // fetches a fresh copy (a short or corrupt cache read must not condemn the sound).
    static constexpr U8  MAX_SOUND_ATTEMPTS = 3;
    static constexpr F64 BAD_SOUND_TTL = 300.0;
    void noteFetchFailed(const LLUUID& id, const std::string& why, bool not_retryable);
    void noteDecodeFailed(const LLUUID& id, const std::string& why);
    void noteDecodeSucceeded(const LLUUID& id);
    // A decoded file exists on disk: make sure the data's flags say so.
    void noteDecodedFilePresent(const LLUUID& id);

    // Internet stream implementation manipulation
    LLStreamingAudioInterface *getStreamingAudioImpl();
    void setStreamingAudioImpl(LLStreamingAudioInterface *impl);
    // Internet stream methods - these will call down into the *mStreamingAudioImpl if it exists
    void startInternetStream(const std::string& url);
    void stopInternetStream();
    void pauseInternetStream(S32 pause);
    void updateInternetStream(); // expected to be called often
    LLAudioPlayState isInternetStreamPlaying();
    // use a value from 0.0 to 1.0, inclusive
    void setInternetStreamGain(F32 vol);
    std::string getInternetStreamURL();

    // For debugging usage
    virtual LLVector3 getListenerPos();
    // The listener's orientation (unit, the global / agent axes; zero without a listener).
    LLVector3 getListenerAt();
    LLVector3 getListenerUp();

    LLAudioBuffer *getFreeBuffer(); // Get a free buffer, or flush an existing one if you have to.
    LLAudioChannel *getFreeChannel(const F32 priority); // Get a free channel or flush an existing one if your priority is higher
    void cleanupBuffer(LLAudioBuffer *bufferp);

    bool hasDecodedFile(const LLUUID &uuid);
    bool hasLocalFile(const LLUUID &uuid);
    // The sound cache dropped this sound's decoded file (LRU eviction / clear).
    void handleDecodedFileEvicted(const LLUUID &uuid);

    // Loads the buffer if decoded, else queues the decode if the asset is local (urgent =
    // front of the decode queue: a sound wanted now, not a preload). False = nothing local.
    bool updateBufferForData(LLAudioData *adp, bool urgent = true);


    // Asset callback when we're retrieved a sound from the asset server.
    void startNextTransfer();
    static void assetCallback(const LLUUID &uuid, LLAssetType::EType type, void *user_data, S32 result_code, LLExtStat ext_status);

    friend class LLPipeline; // For debugging
public:
    void noteSourceQueued() { mQueuedHint = true; }   // an LLAudioSource got queued data
    F32 mMaxWindGain; // Hack.  Public to set before fade in?

protected:
    virtual LLAudioBuffer *createBuffer() = 0;
    virtual LLAudioChannel *createChannel() = 0;

    virtual bool initWind() = 0;
    virtual void cleanupWind() = 0;
    virtual void setInternalGain(F32 gain) = 0;

    void commitDeferredChanges();

    virtual void allocateListener() = 0;


    // listener methods
    virtual void setListenerPos(LLVector3 vec);
    virtual void setListenerVelocity(LLVector3 vec);
    virtual void orientListener(LLVector3 up, LLVector3 at);
    virtual void translateListener(LLVector3 vec);


    F64 mapWindVecToGain(LLVector3 wind_vec);
    F64 mapWindVecToPitch(LLVector3 wind_vec);
    F64 mapWindVecToPan(LLVector3 wind_vec);

protected:
    LLListener *mListenerp;

    bool mMuted;
    void* mUserData;

    S32 mLastStatus;

    bool mEnableWind;

    // Sound assets currently being fetched. Several run at once (bounded), each with its
    // own timeout so a transfer that never calls back only holds one slot.
    struct Transfer
    {
        LLUUID id;
        LLFrameTimer timer;
    };
    std::vector<Transfer> mTransfers;
    static constexpr size_t MAX_CONCURRENT_TRANSFERS = 8;
    bool isTransferInFlight(const LLUUID& id) const;
    // #891: not local, not failed/bad, past its retry time, not in flight, not blocked.
    bool wantsFetch(const LLAudioData* adp, F64 now) const;
    // #891: some source's current or queued clip (decode it first).
    bool isWantedNow(const LLUUID& id) const;
    LLUUID pickNextTransferCandidate();
    LLFrameTimer mTransferIdleScanTimer;
    // set when a source gets queued data; cleared once a pass over the sources finds none
    bool mQueuedHint = false;

    // #891: move a source's queued clip to current once it has a buffer (else wait).
    void advanceQueue(LLAudioSource* sourcep);
    // #891: a source is being deleted by idle(): record why its clip never played.
    void noteSourceEnded(LLAudioSource* sourcep);
    void noteFailure(LLAudioData* adp, const std::string& why, bool not_retryable);
    void markBad(LLAudioData* adp, const std::string& why);
    // Clears an expired bad mark (and the failure count); true if it was expired.
    bool expireBadMark(LLAudioData* adp, F64 now);
    // Frees LLAudioData nothing uses (no source, buffer, fetch or decode) after a grace.
    void sweepAudioData();
    LLFrameTimer mDataSweepTimer;
    size_t mAudioDataPeak = 0;
    U64 mIssueCounts[ISSUE_COUNT] = {};
    bool mIssuesDirty = false;
    LLFrameTimer mIssueSummaryTimer;

    // A list of all audio sources that are known to the viewer at this time.
    // This is most likely a superset of the ones that we actually have audio
    // data for, or are playing back.
    typedef std::map<LLUUID, LLAudioSource *> source_map;
    typedef std::map<LLUUID, LLAudioData *> data_map;

    source_map mAllSources;
    data_map mAllData;

    std::array<LLAudioChannel*, LL_MAX_AUDIO_CHANNELS> mChannels;

    // Buffers needs to change into a different data structure, as the number of buffers
    // that we have active should be limited by RAM usage, not count.
    std::array<LLAudioBuffer*, LL_MAX_AUDIO_BUFFERS> mBuffers;

    F32 mMasterGain;
    F32 mOutputBoost = 1.f;     // #1122 (setOutputBoost)
    F32 mInternalGain;          // Actual gain set; mMasterGain x mOutputBoost, or 0 when mMuted is true.
    F32 mSecondaryGain[AUDIO_TYPE_COUNT];

    F32 mNextWindUpdate;

    LLFrameTimer mWindUpdateTimer;

private:
    void setDefaults();
    LLStreamingAudioInterface *mStreamingAudioImpl;

public:
    // #891: "corrupt" = marked bad (out of retries) and the mark has not expired. The mark
    // lives on the LLAudioData (single source of truth); this replaced LL's session-long
    // mCorruptData counter.
    void markSoundCorrupt(LLUUID const&);
    bool isCorruptSound(LLUUID const&);

};




//
// Standard audio source.  Can be derived from for special sources, such as those attached to objects.
//


class LLAudioSource
{
public:
    // owner_id is the id of the agent responsible for making this sound
    // play, for example, the owner of the object currently playing it
    LLAudioSource(const LLUUID &id, const LLUUID& owner_id, const F32 gain, const S32 type = LLAudioEngine::AUDIO_TYPE_NONE);
    virtual ~LLAudioSource();

    virtual void update();                      // Update this audio source
    void updatePriority();

    void preload(const LLUUID &audio_id); // Only used for preloading UI sounds, now.

    void addAudioData(LLAudioData *adp, bool set_current = true);

    void setForcedPriority(const bool ambient)                      { mForcedPriority = ambient; }
    bool isForcedPriority() const                                   { return mForcedPriority; }

    void setLoop(const bool loop)                           { mLoop = loop; }
    bool isLoop() const                                     { return mLoop; }

    void setSyncMaster(const bool master)                   { mSyncMaster = master; if (master) sAnySyncMaster = true; }
    bool isSyncMaster() const                               { return mSyncMaster; }

    void setSyncSlave(const bool slave)                     { mSyncSlave = slave; }
    bool isSyncSlave() const                                { return mSyncSlave; }

    void setQueueSounds(const bool queue)                   { mQueueSounds = queue; }
    bool isQueueSounds() const                              { return mQueueSounds; }

    void setPlayedOnce(const bool played_once)              { mPlayedOnce = played_once; }

    void setType(S32 type)                                  { mType = type; }
    S32 getType() const                                     { return mType; }

    void setPositionGlobal(const LLVector3d &position_global)       { mPositionGlobal = position_global; }
    LLVector3d getPositionGlobal() const                            { return mPositionGlobal; }
    LLVector3 getVelocity() const                                   { return mVelocity; }
    F32 getPriority() const                                         { return mPriority; }

    // Gain should always be clamped between 0 and 1.
    F32 getGain() const                                             { return mGain; }
    virtual void setGain(const F32 gain)                            { mGain = llclamp(gain, 0.f, 1.f); }

    const LLUUID &getID() const     { return mID; }
    bool isDone() const;
    bool isMuted() const { return mSourceMuted; }

    LLAudioData *getCurrentData();
    LLAudioData *getQueuedData();
    LLAudioBuffer *getCurrentBuffer();

    bool setupChannel();

    // Stop the audio source, reset audio id even if muted
    void stop();

    // #654: silence this source because its sound is blocked, keeping its data id so a
    // later unblock can restart it (sources that recompute their mute state each frame do).
    void muteForBlock();

    // Start the audio source playing,
    // takes mute into account to preserve previous id if nessesary
    bool play(const LLUUID &audio_id);

    bool hasPendingPreloads() const;    // Has preloads that haven't been done yet

    // #891: the current clip has played, can never play, or waited too long to start.
    bool currentClipOver() const;
    // #891: restart the current clip (unmute / unblock) without touching the queue.
    // play(current id) re-queued the same clip when queueing was on.
    void restartCurrent();

    friend class LLAudioEngine;
    friend class LLAudioChannel;
protected:
    void setChannel(LLAudioChannel *channelp);
    LLAudioChannel *getChannel() const                      { return mChannelp; }

public:
    // True while an audio channel is assigned and actually playing (SLSoundLog "playing" column).
    bool isChannelPlaying() const;
protected:
    // #891: every change of the current clip goes through here: "played once" and the
    // start timer belong to the clip, not to the source.
    void setCurrentData(LLAudioData* adp);
    // Drop finished (decoded / failed) preloads; re-request decodes for cached ones.
    void prunePreloads();

protected:
    LLUUID          mID; // The ID of the source is that of the object if it's attached to an object.
    LLUUID          mOwnerID;   // owner of the object playing the sound
    F32             mPriority;
    F32             mGain;
    bool            mSourceMuted;
    bool            mForcedPriority; // ignore mute, set high priority, researved for sound preview and UI
    bool            mLoop;
    bool            mSyncMaster;
    bool            mSyncSlave;
    bool            mQueueSounds;
    bool            mPlayedOnce;
    bool            mCorrupted;
    S32             mType;
    LLVector3d      mPositionGlobal;
    LLVector3       mVelocity;

    //LLAudioSource *mSyncMasterp;  // If we're a slave, the source that we're synced to.
    LLAudioChannel  *mChannelp;     // If we're currently playing back, this is the channel that we're assigned to.
    LLAudioData     *mCurrentDatap;
    LLAudioData     *mQueuedDatap;

    // Conservative hint (never cleared): lets the engine skip sync-master passes when
    // nothing has ever asked to be a sync master.
    static inline bool sAnySyncMaster = false;

    typedef std::map<LLUUID, LLAudioData *> data_map;
    data_map mPreloadMap;

    LLFrameTimer mAgeTimer;
    LLFrameTimer mCurrentTimer;         // #891: since mCurrentDatap last changed
    LLFrameTimer mPreloadCheckTimer;    // #891: prunePreloads() rate limit
    LLFrameTimer mDecodeNudgeTimer;     // #891: update() decode re-request rate limit
};




//
// Generic metadata about a particular piece of audio data.
// The actual data is handled by the derived LLAudioBuffer classes which are
// derived for each audio engine.
//


class LLAudioData
{
  public:
    LLAudioData(const LLUUID &uuid);
    bool load();

    LLUUID         getID() const { return mID; }
    LLAudioBuffer *getBuffer() const { return mBufferp; }

    bool hasLocalData() const { return mHasLocalData; }
    bool hasDecodedData() const { return mHasDecodedData; }
    bool hasCompletedDecode() const { return mHasCompletedDecode; }
    bool hasDecodeFailed() const { return mHasDecodeFailed; }
    bool hasWAVLoadFailed() const { return mHasWAVLoadFailed; }

    void setHasLocalData(const bool hld) { mHasLocalData = hld; }
    void setHasDecodedData(const bool hdd) { mHasDecodedData = hdd; }
    void setHasCompletedDecode(const bool hcd) { mHasCompletedDecode = hcd; }
    void setHasDecodeFailed(const bool hdf) { mHasDecodeFailed = hdf; }
    void setHasWAVLoadFailed(const bool hwlf) { mHasWAVLoadFailed = hwlf; }

    // #891 retry state
    bool isBad(F64 now) const { return mBadUntil > now; }
    // Can never play as things stand (bad, or failed with no retry pending).
    bool isUnplayable(F64 now) const { return isBad(now) || (mHasCompletedDecode && mHasDecodeFailed); }
    // Still on its way: no buffer yet, nothing says it failed for good.
    bool isLoadPending(F64 now) const { return !mBufferp && !isUnplayable(now); }
    U8 getFailures() const { return mFailures; }

    friend class LLAudioEngine;  // Severe laziness, bad.

  protected:
    LLUUID         mID;
    LLAudioBuffer *mBufferp;             // If this data is being used by the audio system, a pointer to the buffer will be set here.
    bool           mHasLocalData;        // Set true if the encoded sound asset file is available locally
    bool           mHasDecodedData;      // Set true if the decoded sound file is available on disk
    bool           mHasCompletedDecode;  // Set true when the sound is decoded
    bool           mHasDecodeFailed;     // Set true if decoding failed, meaning the sound asset is bad
    bool mHasWAVLoadFailed;  // Set true if loading the decoded WAV file failed, meaning the sound asset should be decoded instead if
                             // possible
    bool mRedecodeAttempted; // Set true once an unreadable decoded file has been thrown away and re-decoded this session

    // #891
    U8   mFailures = 0;      // failed fetches + decodes since the last success / expiry
    U16  mReported = 0;      // ESoundIssue bits already logged for this sound
    F64  mRetryAt = 0.0;     // no fetch before this time (backoff)
    F64  mBadUntil = 0.0;    // marked bad until this time (0 = not bad)
    F64  mLastTouch = 0.0;   // last getAudioData() (sweep grace)
};


//
// Base class for an audio channel, i.e. a channel which is capable of playing back a sound.
// Management of channels is done generically, methods for actually manipulating the channel
// are derived for each audio engine.
//


class LLAudioChannel
{
public:
    LLAudioChannel();
    virtual ~LLAudioChannel();

    virtual void setSource(LLAudioSource *sourcep);
    LLAudioSource *getSource() const            { return mCurrentSourcep; }

    void setSecondaryGain(F32 gain)             { mSecondaryGain = gain; }
    F32 getSecondaryGain()                      { return mSecondaryGain; }

    // #683 acoustics: the direct path's low-pass (EFX AL_LOWPASS_GAIN / GAINHF) the engine
    // applies while acoustics is active; 1 / 1 = unfiltered. Reset when the source changes.
    void setAcousticsDirect(F32 gain, F32 gain_hf) { mAcGain = gain; mAcGainHF = gain_hf; }
    // The same for the room-reverb send (only sound transmitted through an obstruction reaches the room).
    void setAcousticsSend(F32 gain, F32 gain_hf) { mAcSendGain = gain; mAcSendGainHF = gain_hf; }
    F32 getAcousticsSendGain() const            { return mAcSendGain; }
    F32 getAcousticsSendGainHF() const          { return mAcSendGainHF; }
    F32 getAcousticsGain() const                { return mAcGain; }
    F32 getAcousticsGainHF() const              { return mAcGainHF; }
    // #857 / #858, keyed by the SOURCE id they were computed for (a channel that has moved on
    // to another source ignores them until the driver sets the new source's own): the second
    // send to reverb zone `zone` (< 0 = none) through its own low-pass, and the position the
    // engine spatialises the sound at (a virtual position: diffraction pans a sound heard
    // round a corner towards the opening; distance kept by the caller).
    void setAcousticsZone(const LLUUID& src, S32 zone, F32 gain, F32 gain_hf)
    {
        mAcZoneSrc = src; mAcZone = zone; mAcZoneGain = gain; mAcZoneGainHF = gain_hf;
    }
    void setAcousticsPosition(const LLUUID& src, bool on, const LLVector3d& pos_global)
    {
        mAcPosSrc = src; mAcPosOn = on; mAcPos = pos_global;
    }
    // #897 boundary bass for this source (dB of low-shelf boost, 0 = none; keyed like the zone).
    void setAcousticsBass(const LLUUID& src, F32 boost_db)
    {
        mAcBassSrc = src; mAcBassDb = boost_db;
    }
    // #683 early gain: raise this source's level by `boost` (>= 1; 1 = none; keyed like the zone).
    // A low-pass filter cannot boost, so the engine applies it to the source gain (AL_GAIN, with
    // AL_MAX_GAIN raised to fit) and divides the reverb / zone sends by it: they keep their level,
    // the dry path (and the #897 bass send, which follows the dry path) rises.
    void setAcousticsBoost(const LLUUID& src, F32 boost)
    {
        mAcBoostSrc = src; mAcBoost = boost;
    }

    friend class LLAudioEngine;
    friend class LLAudioSource;
protected:
    virtual void play() = 0;
    virtual void playSynced(LLAudioChannel *channelp) = 0;
    virtual void cleanup() = 0;
    virtual bool isPlaying() = 0;
    void setWaiting(const bool waiting)         { mWaiting = waiting; }
    bool isWaiting() const                      { return mWaiting; }

    virtual bool updateBuffer(); // Check to see if the buffer associated with the source changed, and update if necessary.
    virtual void update3DPosition() = 0;
    virtual void updateLoop() = 0; // Update your loop/completion status, for use by queueing/syncing.
protected:
    LLAudioSource   *mCurrentSourcep;
    LLAudioBuffer   *mCurrentBufferp;
    bool            mLoopedThisFrame;
    bool            mWaiting;   // Waiting for sync.
    F32             mSecondaryGain;
    F32             mAcGain = 1.f;      // #683 acoustics direct-path filter
    F32             mAcGainHF = 1.f;
    F32             mAcSendGain = 1.f;  // #683 acoustics reverb-send filter
    F32             mAcSendGainHF = 1.f;
    LLUUID          mAcZoneSrc;         // #857 zone send (valid for this source id only)
    S32             mAcZone = -1;
    F32             mAcZoneGain = 0.f;
    F32             mAcZoneGainHF = 1.f;
    LLUUID          mAcPosSrc;          // #858 virtual position (valid for this source id only)
    bool            mAcPosOn = false;
    LLVector3d      mAcPos;
    LLUUID          mAcBassSrc;         // #897 boundary bass (valid for this source id only)
    F32             mAcBassDb = 0.f;
    LLUUID          mAcBoostSrc;        // #683 early gain (valid for this source id only)
    F32             mAcBoost = 1.f;
};




// Basically an interface class to the engine-specific implementation
// of audio data that's ready for playback.
// Will likely get more complex as we decide to do stuff like real streaming audio.


class LLAudioBuffer
{
public:
    virtual ~LLAudioBuffer() = default;
    virtual bool loadWAV(const std::string& filename) = 0;
    virtual U32 getLength() = 0;

    friend class LLAudioEngine;
    friend class LLAudioChannel;
    friend class LLAudioData;
protected:
    bool mInUse;
    LLAudioData *mAudioDatap;
    LLFrameTimer mLastUseTimer;
};

struct SoundData
{
    LLUUID audio_uuid;
    LLUUID owner_id;
    F32 gain;
    S32 type;
    LLVector3d pos_global;

    SoundData(const LLUUID &audio_uuid,
        const LLUUID& owner_id,
        const F32 gain,
        const S32 type = LLAudioEngine::AUDIO_TYPE_NONE,
        const LLVector3d &pos_global = LLVector3d::zero) :
        audio_uuid(audio_uuid),
        owner_id(owner_id),
        gain(gain),
        type(type),
        pos_global(pos_global)
    {
    }
};


extern LLAudioEngine* gAudiop;

#endif
