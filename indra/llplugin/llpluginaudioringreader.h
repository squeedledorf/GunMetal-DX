/**
 * @file llpluginaudioringreader.h
 * @brief #892: the viewer-side consumer of a media plugin's captured audio (llpluginaudioring.h).
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
 * Main thread only. LLPluginClassMedia attaches it when the ring segment exists and detaches it
 * before the segment goes away, so a reader that is attached always points at mapped memory.
 *
 * What a feeder (phase B: the OpenAL stream speakers) does each audio tick:
 *   reader.updateClock(device_seconds);                 // the consumer clock (AL device clock)
 *   while (queued_in_AL < target)
 *   {
 *       ReadInfo info;
 *       if (!reader.readExact(chunk, kChunkFrames, &info)) break;   // underrun: counted
 *       if (info.discontinuity) ...fade / restart...
 *       ...derive L / R / M / Sub, resample by reader.trimRatio(target_fill)...
 *   }
 * No OpenAL here: this class only moves PCM and keeps the clocks.
 */

#ifndef LL_LLPLUGINAUDIORINGREADER_H
#define LL_LLPLUGINAUDIORINGREADER_H

#include "stdtypes.h"

struct LLPluginAudioRingHeader;

class LLPluginAudioRingReader
{
public:
    struct ReadInfo
    {
        U32  frames = 0;
        bool discontinuity = false;     // the producer flushed (seek / new stream) since the last read
        S64  ptsUs = -1;                // stream time of the first frame read, -1 unknown
    };

    struct Stats
    {
        U64 framesRead = 0;             // delivered to the caller
        U64 framesSkipped = 0;          // dropped by skipTo() / resyncs
        U64 underruns = 0;              // starvation episodes (readExact found too little while playing)
        U64 producerDropped = 0;        // frames the plugin dropped: ring full (consumer too slow)
        U32 discontinuities = 0;        // epoch changes seen
        F64 fillSeconds = 0.0;          // buffered audio, seconds
        F64 driftPpm = 0.0;             // producer rate vs the consumer clock (+: producer fast)
        bool driftValid = false;
    };

    bool attach(void* addr, size_t size);
    void detach();
    bool attached() const { return mHeader != nullptr; }

    U32 sampleRate() const;
    U32 channels() const;               // interleaved samples per frame (v1: 2)
    U32 capacityFrames() const;

    // Frames ready to read now.
    U32 available() const;
    // The producer's state (LLPluginAudioRing::EProducerState).
    U32 producerState() const;
    // Frames the producer has delivered since the ring was set up: written + dropped (a full ring
    // must not read as a stalled producer). The arrival curve the consumer's jitter estimate uses.
    U64 producedFrames() const;
    // The producer's epoch (bumped on flush / seek / new stream).
    U32 producerEpoch() const;

    // Up to `max_frames` interleaved frames (channels() samples each) into `out`.
    U32 read(S16* out, U32 max_frames, ReadInfo* info = nullptr);
    // Exactly `frames` or nothing. false while playing = an underrun (counted once per episode).
    bool readExact(S16* out, U32 frames, ReadInfo* info = nullptr);
    // Drop everything but the newest `keep_frames` (a hard resync). Returns the frames dropped.
    U32 skipTo(U32 keep_frames);

    // The stream clock: stream time (us) of the next frame read() would return. -1: unknown
    // (the producer gave no timestamps yet, or it just flushed).
    S64 streamTimeUs() const;
    // Frames consumed since attach: the consumer's position in the stream, in frames.
    U64 consumedFrames() const { return mStats.framesRead + mStats.framesSkipped; }

    // Feed the consumer clock (seconds, monotonic; phase B: the OpenAL device clock). Call once per
    // tick. Measures the producer's frame rate against it (written + dropped, so a full ring does
    // not bias it) over >= kDriftWindowSecs windows.
    void updateClock(F64 consumer_seconds);
    F64  driftPpm() const { return mStats.driftPpm; }
    bool driftValid() const { return mStats.driftValid; }
    // Suggested consume ratio for a fractional resampler (source frames per output frame), held
    // within +-0.2 %: the measured drift plus a slow pull of the fill level toward `target_fill`.
    F64  trimRatio(U32 target_fill_frames) const;

    const Stats& stats();

    static constexpr F64 kDriftWindowSecs = 10.0;
    static constexpr F64 kMaxTrim = 0.002;

private:
    void checkEpoch(ReadInfo* info);
    void resetDrift();

    LLPluginAudioRingHeader* mHeader = nullptr;
    U64  mReadPos = 0;          // our copy of read_frames
    U32  mEpoch = 0;
    bool mStarved = false;      // inside an underrun episode

    // drift estimator
    bool mDriftAnchored = false;
    bool mDriftWarm = false;    // the first window (plugin pre-buffer burst) is discarded
    F64  mDriftT0 = 0.0;
    U64  mDriftF0 = 0;
    U32  mDriftEpoch = 0;

    Stats mStats;
};

#endif // LL_LLPLUGINAUDIORINGREADER_H
