/**
 * @file slparcelspeakeraudio.h
 * @brief #892 phase B: the parcel stream played by the viewer -- through the in-world speaker
 *        objects as positioned OpenAL sources with room acoustics, or as one stereo bed.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
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
 * $/LicenseInfo$
 *
 * INPUT. With ParcelAudioCapture on, the parcel stream's media plugin plays nothing itself: its
 * PCM (48 kHz stereo S16) lands in a shared ring (llpluginaudioringreader.h). This is the ring's
 * only consumer. ParcelAudioCapture off: nothing here runs and the plugin plays as it always did.
 *
 * FEEDER (main thread, audio idle). Each tick it keeps >= ParcelAudioBufferSeconds (1.2 s)
 * queued in OpenAL: 50 ms chunks read from the ring, resampled by the reader's trim ratio
 * (measured producer drift + a slow pull of the ring toward its adaptive target -- the deepest
 * producer stall seen in the last 2 min + 0.25 s, less the OpenAL queue, at least 150 ms; a (re)start
 * waits for it -- held within +-0.2 %; a
 * 16-tap windowed-sinc polyphase resampler, exact at ratio 1) -- the drift correction is HERE,
 * never per-source pitch, so the speakers cannot drift apart. Each chunk is split ONCE into the
 * roles (stereo bed, M = (L+R)/2, L, R, S = M through a 120 Hz Linkwitz-Riley low-pass) and
 * pushed to the engine's stream timeline (LLAudioEngine::streamPush), which queues the same AL
 * buffer on every source of a role, starts them together, joins late ones sample-exact and
 * restarts all together on any underrun.
 *
 * OUTPUT.
 *  - No speakers (ParcelAudioSpeakers off, or none verified on the parcel): ONE stereo source at
 *    the listener (direct channels, no effects) at the music level -- the plugin's output, now
 *    from OpenAL.
 *  - Speakers: up to ParcelAudioMaxSpeakers (16, max 32) audible ones, ranked by level at the listener
 *    (vol x distance falloff; a playing one keeps its place unless clearly outranked). Each is a
 *    positioned source of its role: full level out to r / 4, then real inverse distance (ref / d,
 *    -6 dB per doubling), r a soft maximum (the last 15 % of r fades to silence); dir= is an
 *    OpenAL cone around the prim's +X; vol= is its gain, 0..2 (OpenAL AL_MAX_GAIN raised to fit;
 *    beyond the cap the quietest speakers are skipped, logged). ParcelAudioDiffuseReverb: the room /
 *    zone sends keep their level over distance (diffuse field); off: they decay like a channel's.
 *    With room acoustics active (AudioAcoustics + ray tracing) each speaker is traced as an extra
 *    acoustics source and gets its occlusion low-pass, room send, reverb zone and diffraction pan
 *    (LLCleanAcoustics::sourceAcoustics); acoustics off: position and falloff only.
 *  - Out of range of every speaker: the parcel's [slspk outside=silent|ambient] policy, or the
 *    user's ParcelAudioSpeakersOutside override; ambient = the stereo bed at -12 dB. Joins,
 *    leaves and the bed crossfade are short gain ramps.
 *  Gains: AudioLevelMusic, MuteMusic and the teleport fade (LLViewerAudio::getFadeVolume) on every
 *  source; the master level and mute reach OpenAL as the listener gain (not applied twice).
 *
 * t0= CLIPS. A fixed clip (the plugin reports a length) whose speakers carry t0= is seeked once
 * per stream / t0 to (synced UTC - t0 + the pipeline latency) mod length (SLWeather::syncedSeconds).
 *
 * LOG. Every 10 s under "ParcelAudio": mode, speakers live / cap / peak, queue and ring fill,
 * drift, trim, underruns (ring and OpenAL), restarts, joins, buffers; mode changes as they happen.
 */

#ifndef SL_PARCELSPEAKERAUDIO_H
#define SL_PARCELSPEAKERAUDIO_H

#include "stdtypes.h"

class SLParcelSpeakerAudio
{
public:
    // Audio idle: after LLCleanAcoustics::update() (this frame's acoustics answers), before
    // gAudiop->idle().
    static void update();
    // Before the audio engine is shut down: free every engine source and buffer.
    static void shutdown();

    enum EMode : U8
    {
        MODE_OFF = 0,           // not capturing: the plugin plays the stream itself (or no stream)
        MODE_STEREO,            // captured, no speakers: the stereo bed at the music level
        MODE_SPEAKERS,          // in range of at least one speaker
        MODE_OUTSIDE_AMBIENT,   // speakers exist, none in range: the bed at -12 dB
        MODE_OUTSIDE_SILENT,    // speakers exist, none in range: silence
    };
    static EMode mode();
    static const char* modeName(EMode m);
};

#endif
