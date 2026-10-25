/**
 * @file llpluginaudioring.h
 * @brief #892: the shared-memory PCM ring a media plugin writes its decoded audio into, when
 *        the viewer asks it to ("audio capture"). Header-only: the plugin process (producer)
 *        and the viewer (consumer, LLPluginAudioRingReader) both include it.
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
 * LAYOUT. One shared segment (LLPluginProcessParent::addSharedMemory, the "shm_add" mechanism
 * video frames use): an LLPluginAudioRingHeader, then `capacity_frames` interleaved frames.
 * The viewer creates the segment and writes the header; the plugin only writes PCM, the
 * write cursor and its own counters. Single producer (the plugin's audio thread), single
 * consumer (the viewer's main thread).
 *
 * CURSORS. write_frames / read_frames are monotonic 64-bit frame counters (index = counter %
 * capacity), so full and empty are never ambiguous. The producer never overwrites unread
 * frames: when the ring is full it drops the NEW frames and counts them in dropped_frames.
 * The consumer may skip ahead (advance read_frames) at any time to resync.
 *
 * CONTROL. Nothing per chunk goes over the plugin pipe. The pipe carries only:
 *   viewer -> plugin  media "audio_capture"       {enable, test_discard} (queued: arrives before load_uri)
 *   viewer -> plugin  media "audio_capture_shm"   {name, size}      (after shm_add)
 *   plugin -> viewer  media "audio_capture_format"{rate, channels, format, epoch}
 *   plugin -> viewer  media "audio_capture_status"{write_frames, pts_us, dropped, flushes, state}
 *   plugin -> viewer  media "audio_capture_unavailable" {reason}    (capture cannot work: the plugin
 *                                                                    plays the stream itself instead)
 * ICY / stream tags keep using "stream_metadata", unchanged.
 */

#ifndef LL_LLPLUGINAUDIORING_H
#define LL_LLPLUGINAUDIORING_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace LLPluginAudioRing
{
    constexpr uint32_t kMagic   = 0x52414c53u;  // "SLAR"
    constexpr uint32_t kVersion = 1;

    // v1 format: what both plugins convert to (VLC: "S16N"; GStreamer: S16LE caps).
    constexpr uint32_t kRate     = 48000;
    constexpr uint32_t kChannels = 2;
    enum ESampleFormat : uint32_t { FORMAT_S16 = 1 };

    // 8 s at 48 kHz (1.5 MB). The ring is the consumer's jitter buffer: a network stream can stall
    // for seconds and then deliver the backlog at once (playbin keeps playing through a stall, so
    // the late buffers pass the synced appsink unpaced), and VLC delivers in bursts. At 1.5 s such
    // a burst overflowed the ring and was dropped while the consumer starved a moment later. The
    // consumer decides how much of it to keep filled (SLParcelSpeakerAudio: the measured stall
    // depth plus a margin), so the size costs latency only when the delivery needs it.
    constexpr uint32_t kDefaultCapacityFrames = 48000 * 8;

    enum EProducerState : uint32_t
    {
        STATE_IDLE = 0,     // attached, nothing decoded yet
        STATE_PLAYING,
        STATE_PAUSED,
        STATE_STOPPED,      // drained / end of stream
    };
}

// Placed at the start of the segment. Plain integers are written once by the viewer before the
// plugin learns the segment's name; everything that changes afterwards is atomic.
struct LLPluginAudioRingHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t header_bytes;      // offset of the PCM data from the segment start
    uint32_t capacity_frames;
    uint32_t sample_rate;
    uint32_t channels;
    uint32_t sample_format;     // LLPluginAudioRing::ESampleFormat
    uint32_t reserved0;

    std::atomic<uint64_t> write_frames;     // producer: frames written, total
    std::atomic<uint64_t> read_frames;      // consumer: frames consumed (or skipped), total
    std::atomic<int64_t>  write_pts_us;     // producer: stream time of the frame AT write_frames (-1: unknown)
    std::atomic<uint64_t> dropped_frames;   // producer: frames dropped because the ring was full
    std::atomic<uint32_t> epoch;            // producer: bumped on flush / seek / discontinuity
    std::atomic<uint32_t> producer_state;   // LLPluginAudioRing::EProducerState
    std::atomic<uint32_t> flushes;          // producer: flush count (diagnostics)
    uint32_t reserved1;

    static constexpr size_t frameBytes(uint32_t channels) { return sizeof(int16_t) * channels; }
    static constexpr size_t dataOffset() { return (sizeof(LLPluginAudioRingHeader) + 63) & ~size_t(63); }
    static constexpr size_t segmentBytes(uint32_t capacity_frames, uint32_t channels)
    {
        return dataOffset() + size_t(capacity_frames) * frameBytes(channels);
    }

    int16_t* data() { return reinterpret_cast<int16_t*>(reinterpret_cast<uint8_t*>(this) + header_bytes); }
    const int16_t* data() const { return reinterpret_cast<const int16_t*>(reinterpret_cast<const uint8_t*>(this) + header_bytes); }

    // Viewer: initialise a freshly created segment of `size` bytes. false: too small.
    static LLPluginAudioRingHeader* initSegment(void* addr, size_t size,
                                                uint32_t capacity_frames = LLPluginAudioRing::kDefaultCapacityFrames)
    {
        if (!addr || size < segmentBytes(capacity_frames, LLPluginAudioRing::kChannels))
            return nullptr;
        std::memset(addr, 0, size);
        LLPluginAudioRingHeader* h = new (addr) LLPluginAudioRingHeader();
        h->version = LLPluginAudioRing::kVersion;
        h->header_bytes = (uint32_t)dataOffset();
        h->capacity_frames = capacity_frames;
        h->sample_rate = LLPluginAudioRing::kRate;
        h->channels = LLPluginAudioRing::kChannels;
        h->sample_format = LLPluginAudioRing::FORMAT_S16;
        h->write_frames.store(0, std::memory_order_relaxed);
        h->read_frames.store(0, std::memory_order_relaxed);
        h->write_pts_us.store(-1, std::memory_order_relaxed);
        h->dropped_frames.store(0, std::memory_order_relaxed);
        h->epoch.store(0, std::memory_order_relaxed);
        h->producer_state.store(LLPluginAudioRing::STATE_IDLE, std::memory_order_relaxed);
        h->flushes.store(0, std::memory_order_relaxed);
        // magic last: a segment with a valid magic is fully initialised.
        std::atomic_thread_fence(std::memory_order_release);
        h->magic = LLPluginAudioRing::kMagic;
        return h;
    }

    // Either side: the segment holds a ring this code understands. `size`: the mapped size.
    static LLPluginAudioRingHeader* validate(void* addr, size_t size)
    {
        if (!addr || size < sizeof(LLPluginAudioRingHeader))
            return nullptr;
        LLPluginAudioRingHeader* h = reinterpret_cast<LLPluginAudioRingHeader*>(addr);
        if (h->magic != LLPluginAudioRing::kMagic || h->version != LLPluginAudioRing::kVersion)
            return nullptr;
        if (h->channels == 0 || h->channels > 8 || h->capacity_frames == 0 || h->header_bytes < sizeof(LLPluginAudioRingHeader)
            || size < size_t(h->header_bytes) + size_t(h->capacity_frames) * frameBytes(h->channels))
            return nullptr;
        return h;
    }
};

// Cross-process atomics must not fall back to a lock that lives in one process only.
static_assert(std::atomic<uint64_t>::is_always_lock_free, "audio ring needs lock-free 64-bit atomics");
static_assert(std::atomic<int64_t>::is_always_lock_free, "audio ring needs lock-free 64-bit atomics");
static_assert(std::atomic<uint32_t>::is_always_lock_free, "audio ring needs lock-free 32-bit atomics");

// Producer side (the plugin). Not thread-safe by itself: the plugin serialises write() with
// attach() / detach() (its shm_remove handler) under its own mutex.
class LLPluginAudioRingWriter
{
public:
    bool attach(void* addr, size_t size)
    {
        mHeader = LLPluginAudioRingHeader::validate(addr, size);
        if (mHeader && (mHeader->sample_format != LLPluginAudioRing::FORMAT_S16
                        || mHeader->sample_rate != LLPluginAudioRing::kRate
                        || mHeader->channels != LLPluginAudioRing::kChannels))
        {
            mHeader = nullptr;  // a format this producer does not convert to
        }
        mAddress = mHeader ? addr : nullptr;
        return mHeader != nullptr;
    }
    void detach() { mHeader = nullptr; mAddress = nullptr; }
    bool attached() const { return mHeader != nullptr; }

    // #960 test hook (the viewer's ParcelAudioCaptureTestFail): attached and "playing", but every
    // frame is thrown away uncounted, so the viewer sees exactly a capture that delivers nothing.
    void setTestDiscard(bool discard) { mTestDiscard = discard; }
    const void* address() const { return mAddress; }

    // Interleaved S16 frames (kChannels per frame). pts_us: stream time of the first frame, or -1.
    // Returns the frames actually stored (the rest were dropped: ring full).
    uint32_t write(const int16_t* frames, uint32_t count, int64_t pts_us)
    {
        if (!mHeader || !frames || count == 0)
            return 0;
        if (mTestDiscard)
        {
            mHeader->producer_state.store(LLPluginAudioRing::STATE_PLAYING, std::memory_order_relaxed);
            return 0;
        }
        LLPluginAudioRingHeader& h = *mHeader;
        const uint64_t cap = h.capacity_frames;
        const uint64_t w = h.write_frames.load(std::memory_order_relaxed);
        const uint64_t r = h.read_frames.load(std::memory_order_acquire);
        const uint64_t used = (w >= r) ? (w - r) : 0;   // (r > w only if the consumer misbehaves)
        const uint64_t room = (used < cap) ? (cap - used) : 0;
        const uint32_t n = (uint32_t)((count < room) ? count : room);
        if (n < count)
            h.dropped_frames.fetch_add(count - n, std::memory_order_relaxed);
        if (n > 0)
        {
            const uint32_t ch = h.channels;
            const uint64_t at = w % cap;
            const uint64_t first = (n < cap - at) ? n : (cap - at);
            int16_t* data = h.data();
            std::memcpy(data + at * ch, frames, size_t(first) * ch * sizeof(int16_t));
            if (first < n)
                std::memcpy(data, frames + first * ch, size_t(n - first) * ch * sizeof(int16_t));
            if (pts_us >= 0)
                h.write_pts_us.store(pts_us + (int64_t)n * 1000000 / h.sample_rate, std::memory_order_relaxed);
            h.write_frames.store(w + n, std::memory_order_release);
        }
        h.producer_state.store(LLPluginAudioRing::STATE_PLAYING, std::memory_order_relaxed);
        return n;
    }

    void setState(uint32_t state)
    {
        if (mHeader) mHeader->producer_state.store(state, std::memory_order_relaxed);
    }

    // A discontinuity (seek, flush, new stream): the consumer sees the epoch change and resyncs.
    void flush()
    {
        if (!mHeader) return;
        mHeader->write_pts_us.store(-1, std::memory_order_relaxed);
        mHeader->flushes.fetch_add(1, std::memory_order_relaxed);
        mHeader->epoch.fetch_add(1, std::memory_order_release);
    }

    uint64_t writeFrames() const { return mHeader ? mHeader->write_frames.load(std::memory_order_relaxed) : 0; }
    int64_t  writePtsUs() const { return mHeader ? mHeader->write_pts_us.load(std::memory_order_relaxed) : -1; }
    uint64_t droppedFrames() const { return mHeader ? mHeader->dropped_frames.load(std::memory_order_relaxed) : 0; }
    uint32_t epoch() const { return mHeader ? mHeader->epoch.load(std::memory_order_relaxed) : 0; }
    uint32_t flushes() const { return mHeader ? mHeader->flushes.load(std::memory_order_relaxed) : 0; }
    uint32_t state() const { return mHeader ? mHeader->producer_state.load(std::memory_order_relaxed) : 0; }

private:
    LLPluginAudioRingHeader* mHeader = nullptr;
    void* mAddress = nullptr;
    bool mTestDiscard = false;
};

#endif // LL_LLPLUGINAUDIORING_H
