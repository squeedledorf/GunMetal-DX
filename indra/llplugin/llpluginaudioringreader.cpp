/**
 * @file llpluginaudioringreader.cpp
 * @brief #892: viewer-side consumer of a media plugin's captured audio. See the header.
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
 */

#include "linden_common.h"

#include "llpluginaudioringreader.h"
#include "llpluginaudioring.h"

#include <algorithm>
#include <cmath>
#include <cstring>

bool LLPluginAudioRingReader::attach(void* addr, size_t size)
{
    detach();
    mHeader = LLPluginAudioRingHeader::validate(addr, size);
    if (!mHeader)
        return false;
    mReadPos = mHeader->read_frames.load(std::memory_order_relaxed);
    mEpoch = mHeader->epoch.load(std::memory_order_acquire);
    return true;
}

void LLPluginAudioRingReader::detach()
{
    mHeader = nullptr;
    mReadPos = 0;
    mEpoch = 0;
    mStarved = false;
    resetDrift();
    mStats = Stats();
}

U32 LLPluginAudioRingReader::sampleRate() const { return mHeader ? mHeader->sample_rate : 0; }
U32 LLPluginAudioRingReader::channels() const { return mHeader ? mHeader->channels : 0; }
U32 LLPluginAudioRingReader::capacityFrames() const { return mHeader ? mHeader->capacity_frames : 0; }

U32 LLPluginAudioRingReader::producerState() const
{
    return mHeader ? mHeader->producer_state.load(std::memory_order_relaxed) : (U32)LLPluginAudioRing::STATE_IDLE;
}

U64 LLPluginAudioRingReader::producedFrames() const
{
    if (!mHeader)
        return 0;
    return mHeader->write_frames.load(std::memory_order_acquire) + mHeader->dropped_frames.load(std::memory_order_relaxed);
}

U32 LLPluginAudioRingReader::producerEpoch() const
{
    return mHeader ? mHeader->epoch.load(std::memory_order_acquire) : 0;
}

U32 LLPluginAudioRingReader::available() const
{
    if (!mHeader)
        return 0;
    const U64 w = mHeader->write_frames.load(std::memory_order_acquire);
    if (w <= mReadPos)
        return 0;
    // (The producer never writes past capacity unread frames; clamp anyway against a bad peer.)
    return (U32)std::min<U64>(w - mReadPos, mHeader->capacity_frames);
}

void LLPluginAudioRingReader::checkEpoch(ReadInfo* info)
{
    const U32 epoch = mHeader->epoch.load(std::memory_order_acquire);
    if (epoch != mEpoch)
    {
        mEpoch = epoch;
        ++mStats.discontinuities;
        resetDrift();
        if (info)
            info->discontinuity = true;
    }
}

U32 LLPluginAudioRingReader::read(S16* out, U32 max_frames, ReadInfo* info)
{
    if (info)
        *info = ReadInfo();
    if (!mHeader || !out || max_frames == 0)
        return 0;
    checkEpoch(info);

    const U32 n = std::min(available(), max_frames);
    if (n == 0)
        return 0;
    if (info)
        info->ptsUs = streamTimeUs();

    const U32 ch = mHeader->channels;
    const U64 cap = mHeader->capacity_frames;
    const U64 at = mReadPos % cap;
    const U64 first = std::min<U64>(n, cap - at);
    const S16* data = mHeader->data();
    std::memcpy(out, data + at * ch, size_t(first) * ch * sizeof(S16));
    if (first < n)
        std::memcpy(out + first * ch, data, size_t(n - first) * ch * sizeof(S16));

    mReadPos += n;
    // Release: the producer may reuse these frames only after our copy is complete.
    mHeader->read_frames.store(mReadPos, std::memory_order_release);
    mStats.framesRead += n;
    if (info)
        info->frames = n;
    return n;
}

bool LLPluginAudioRingReader::readExact(S16* out, U32 frames, ReadInfo* info)
{
    if (info)
        *info = ReadInfo();
    if (!mHeader || frames == 0)
        return false;
    if (available() < frames)
    {
        // Only a starving PLAYING producer is an underrun; idle / paused / ended is just silence.
        if (!mStarved && producerState() == LLPluginAudioRing::STATE_PLAYING)
        {
            mStarved = true;
            ++mStats.underruns;
        }
        return false;
    }
    mStarved = false;
    return read(out, frames, info) == frames;
}

U32 LLPluginAudioRingReader::skipTo(U32 keep_frames)
{
    if (!mHeader)
        return 0;
    const U32 avail = available();
    if (avail <= keep_frames)
        return 0;
    const U32 drop = avail - keep_frames;
    mReadPos += drop;
    mHeader->read_frames.store(mReadPos, std::memory_order_release);
    mStats.framesSkipped += drop;
    return drop;
}

S64 LLPluginAudioRingReader::streamTimeUs() const
{
    if (!mHeader || mHeader->sample_rate == 0)
        return -1;
    const S64 pts_at_write = mHeader->write_pts_us.load(std::memory_order_relaxed);
    if (pts_at_write < 0)
        return -1;
    return pts_at_write - (S64)available() * 1000000 / (S64)mHeader->sample_rate;
}

void LLPluginAudioRingReader::resetDrift()
{
    mDriftAnchored = false;
    mDriftWarm = false;
}

void LLPluginAudioRingReader::updateClock(F64 consumer_seconds)
{
    if (!mHeader || mHeader->sample_rate == 0)
        return;
    const U32 epoch = mHeader->epoch.load(std::memory_order_acquire);
    const bool playing = producerState() == LLPluginAudioRing::STATE_PLAYING;
    // Produced = written + dropped: a full ring (slow consumer) must not read as a slow producer.
    const U64 produced = mHeader->write_frames.load(std::memory_order_acquire)
                         + mHeader->dropped_frames.load(std::memory_order_relaxed);

    if (!playing || epoch != mDriftEpoch || !mDriftAnchored || consumer_seconds < mDriftT0)
    {
        mDriftAnchored = playing;
        mDriftWarm = false;
        mDriftT0 = consumer_seconds;
        mDriftF0 = produced;
        mDriftEpoch = epoch;
        return;
    }
    const F64 dt = consumer_seconds - mDriftT0;
    if (dt < kDriftWindowSecs)
        return;
    const F64 rate = (F64)(produced - mDriftF0) / dt;
    const F64 ppm = (rate / (F64)mHeader->sample_rate - 1.0) * 1e6;
    mDriftT0 = consumer_seconds;
    mDriftF0 = produced;
    if (!mDriftWarm)
    {
        mDriftWarm = true;      // the first window holds the plugin's pre-buffer burst
        return;
    }
    // A real clock mismatch is tens of ppm; > 2 % is a stall or a burst, not drift.
    if (!std::isfinite(ppm) || std::fabs(ppm) > 20000.0)
        return;
    if (!mStats.driftValid)
    {
        mStats.driftPpm = ppm;
        mStats.driftValid = true;
    }
    else
    {
        mStats.driftPpm += 0.2 * (ppm - mStats.driftPpm);
    }
}

F64 LLPluginAudioRingReader::trimRatio(U32 target_fill_frames) const
{
    if (!mHeader || mHeader->sample_rate == 0)
        return 1.0;
    const F64 drift = mStats.driftValid ? mStats.driftPpm * 1e-6 : 0.0;
    // Fill error in seconds, closed over ~20 s: a 100 ms excess asks for +0.5 % (clamped below).
    const F64 err_s = ((F64)available() - (F64)target_fill_frames) / (F64)mHeader->sample_rate;
    const F64 ratio = 1.0 + drift + err_s / 20.0;
    return std::clamp(ratio, 1.0 - kMaxTrim, 1.0 + kMaxTrim);
}

const LLPluginAudioRingReader::Stats& LLPluginAudioRingReader::stats()
{
    if (mHeader && mHeader->sample_rate)
    {
        mStats.fillSeconds = (F64)available() / (F64)mHeader->sample_rate;
        mStats.producerDropped = mHeader->dropped_frames.load(std::memory_order_relaxed);
    }
    return mStats;
}
