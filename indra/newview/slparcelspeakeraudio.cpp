/**
 * @file slparcelspeakeraudio.cpp
 * @brief #892 phase B: the parcel stream through in-world speakers. See the header.
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

#include "llviewerprecompiledheaders.h"

#include "slparcelspeakeraudio.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llaudioengine.h"
#include "llcleanacoustics.h"
#include "llpluginaudioring.h"
#include "llpluginaudioringreader.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llvieweraudio.h"
#include "llviewermedia_streamingaudio.h"
#include "slparcelspeakers.h"
#include "slweather.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <map>
#include <vector>

namespace
{
// The ring is the jitter buffer in front of OpenAL. Its target fill adapts to the producer's
// delivery: the deepest stall seen (the arrival curve's drawdown) over the last kJitterWindowSecs,
// plus a margin, less what OpenAL already holds -- never below kRingFloorSecs (a steady producer:
// ~1.35 s total, as before). The trim pulls the fill toward it; a group (re)start waits for it.
constexpr F64 kRingFloorSecs = 0.15;     // the least the ring is asked to hold
constexpr F64 kJitterMarginSecs = 0.25;  // held beyond the deepest stall seen
constexpr U32 kJitterWindowSecs = 120;   // stalls are remembered this long (1 s buckets)
constexpr F64 kJitterLeak = 0.002;       // the arrival peak relaxes 2 ms per s (rate error, drift)
constexpr F64 kSkipExcessSecs = 0.5;     // a ring this far above its target...
constexpr F64 kSkipHoldSecs = 20.0;      // ...for this long without dipping is skipped down to it
constexpr F64 kPrebufferSecs = 1.0;      // queued in OpenAL before the group starts
constexpr U32 kMaxChunksPerTick = 40;    // (2 s: the first fill after a start / an underrun)
constexpr F32 kAmbientGain = 0.2512f;    // -12 dB
constexpr F32 kSpeakerFadeSecs = 0.15f;  // join / leave ramp
constexpr F32 kBedFadeSecs = 0.5f;       // bed crossfade
constexpr F32 kKeepBonus = 1.5f;         // a playing speaker keeps its place unless outranked by this (+3.5 dB)
// A speaker that leaves the playing set fades to silence but keeps its source -- still running on
// the shared timeline -- this long, so coming back (cap churn, walking along a row) is a fade-in,
// not a re-join. At most kKeepAliveExtra such silent sources beyond the cap.
constexpr F64 kKeepAliveSecs = 20.0;
constexpr U32 kKeepAliveExtra = 8;
constexpr U32 kMaxSpeakers = 32;         // ParcelAudioMaxSpeakers' ceiling (acoustics budget: kMaxExtra)
// Full level out to r / 4, then real inverse distance: ref / d (-6 dB per doubling; OpenAL inverse
// distance clamped, reference r / 4, rolloff 1 like every channel). r= is a soft maximum: the last
// 15 % of it fades to silence (LLAudioEngine::softEdge). (r / 25 was physical for a small box but
// far too quiet for a stage: a whole wall of speakers to reach normal volume.)
// r = 100 (ref 25 m): 0 dB to 25 m, -1.6 dB at 30, -7.6 at 60, -10.6 at 85, fading out to 100 m.
// r = 30 (ref 7.5 m): 0 dB to 7.5 m, -2.5 dB at 10, -10.6 at 25.5, fading out to 30 m.
constexpr F32 kRefPerRange = 0.25f;
F32 refDistance(F32 range) { return std::max(range * kRefPerRange, 1.f); }
constexpr F32 kConeOuterGain = 0.1f;     // behind a directional box: -20 dB
constexpr F32 kConeOuterExtra = 90.f;    // the outer cone is this much wider than dir=
constexpr F64 kLogInterval = 10.0;
// A namespace mixed into speaker ids: the acoustics key of a speaker is never the key of a
// sound its object plays on a channel.
const LLUUID kAcousticsNs("6c3f2a9e-8920-4b1d-9a55-51a5bea7e892");
constexpr F64 kPi = 3.14159265358979323846;
constexpr F64 kSqrtHalf = 0.70710678118654752440;

// ---- resampler: 16-tap windowed sinc, 512 phases (linear between rows); exact at ratio 1 ----
class Resampler
{
public:
    static constexpr S32 kTaps = 16;
    static constexpr S32 kHist = kTaps / 2 - 1;     // frames before the read position (7)
    static constexpr S32 kPhases = 512;

    Resampler()
    {
        mTable.resize((size_t)(kPhases + 1) * kTaps);
        for (S32 p = 0; p <= kPhases; ++p)
        {
            const F64 frac = (F64)p / kPhases;
            F64 sum = 0.0;
            F32* row = &mTable[(size_t)p * kTaps];
            for (S32 k = 0; k < kTaps; ++k)
            {
                const F64 t = (F64)(k - kHist) - frac;       // input tap's offset from the read position
                const F64 x = kPi * t;
                const F64 sinc = std::fabs(t) < 1e-9 ? 1.0 : std::sin(x) / x;
                const F64 u = t / (kTaps / 2);               // Blackman over |t| <= 8
                const F64 w = std::fabs(u) >= 1.0 ? 0.0 : 0.42 + 0.5 * std::cos(kPi * u) + 0.08 * std::cos(2.0 * kPi * u);
                row[k] = (F32)(sinc * w);
                sum += sinc * w;
            }
            for (S32 k = 0; k < kTaps; ++k) row[k] = (F32)(row[k] / sum);   // unity DC gain
        }
        reset();
    }

    void reset()
    {
        mIn.assign((size_t)kHist * 2, 0.f);
        mPos = kHist;
    }

    // Input frames still needed to produce `n` output frames at `ratio`.
    U32 need(U32 n, F64 ratio) const
    {
        const F64 last = mPos + (F64)(n - 1) * ratio;
        const U64 req = (U64)std::floor(last) + (U64)(kTaps - kHist);   // last tap index + 1
        const U64 have = mIn.size() / 2;
        return req > have ? (U32)(req - have) : 0;
    }

    void push(const S16* pcm, U32 frames)
    {
        const size_t at = mIn.size();
        mIn.resize(at + (size_t)frames * 2);
        for (size_t i = 0; i < (size_t)frames * 2; ++i) mIn[at + i] = (F32)pcm[i] * (1.f / 32768.f);
    }

    // `n` interleaved stereo frames into `out`. Call need() / push() first.
    void produce(F32* out, U32 n, F64 ratio)
    {
        const S64 frames = (S64)(mIn.size() / 2);
        for (U32 i = 0; i < n; ++i)
        {
            const F64 x = mPos + (F64)i * ratio;
            const S64 ip = (S64)std::floor(x);
            const F64 ph = (x - (F64)ip) * kPhases;
            S32 p0 = (S32)ph;
            if (p0 >= kPhases) p0 = kPhases - 1;
            const F32 t = (F32)(ph - p0);
            const F32* h0 = &mTable[(size_t)p0 * kTaps];
            const F32* h1 = h0 + kTaps;
            const S64 base = ip - kHist;
            F32 l = 0.f, r = 0.f;
            for (S32 k = 0; k < kTaps; ++k)
            {
                const S64 idx = base + k;
                if (idx < 0 || idx >= frames) continue;     // (never with need() honoured)
                const F32 h = h0[k] + (h1[k] - h0[k]) * t;
                l += mIn[(size_t)idx * 2] * h;
                r += mIn[(size_t)idx * 2 + 1] * h;
            }
            out[i * 2] = l;
            out[i * 2 + 1] = r;
        }
        mPos += (F64)n * ratio;
        // Keep kHist frames of history before the read position.
        const S64 drop = (S64)std::floor(mPos) - kHist;
        if (drop > 0)
        {
            const size_t d = std::min<size_t>((size_t)drop, mIn.size() / 2);
            mIn.erase(mIn.begin(), mIn.begin() + (std::ptrdiff_t)(d * 2));
            mPos -= (F64)d;
        }
    }

private:
    std::vector<F32> mTable;
    std::vector<F32> mIn;       // interleaved stereo
    F64 mPos = 0.0;             // read position (frames) in mIn
};

// ---- the sub: Linkwitz-Riley 4th order low-pass (two Butterworth biquads) ----
struct Biquad
{
    F64 b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
    F64 z1 = 0, z2 = 0;
    void lowpass(F64 fc, F64 fs)
    {
        const F64 w0 = 2.0 * kPi * fc / fs;
        const F64 cw = std::cos(w0), alpha = std::sin(w0) / (2.0 * kSqrtHalf);   // Q = 1/sqrt(2)
        const F64 a0 = 1.0 + alpha;
        b0 = (1.0 - cw) * 0.5 / a0;
        b1 = (1.0 - cw) / a0;
        b2 = b0;
        a1 = -2.0 * cw / a0;
        a2 = (1.0 - alpha) / a0;
        z1 = z2 = 0.0;
    }
    F64 run(F64 x)   // transposed direct form II
    {
        const F64 y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

inline S16 toS16(F32 v)
{
    const F32 s = v * 32768.f;
    return (S16)llclamp((S32)std::lrint(s), -32768, 32767);
}

// The producer's arrival curve against the clock: dev(t) = delivered / rate - elapsed x (1 + drift).
// A steady producer keeps it flat; a stall pulls it below its running peak by the audio the
// consumer must hold to ride the stall out. Delivered counts written + dropped, so neither a full
// ring nor a viewer hitch (the counters keep running) reads as a stall.
struct ArrivalJitter
{
    bool  anchored = false;
    F64   t0 = 0.0, lastT = 0.0;
    U64   f0 = 0;
    U32   epoch = 0;
    F64   peak = 0.0;
    F64   bucketStart = -1.0, bucketMax = 0.0;
    std::deque<F32> buckets;    // deepest stall per second (s), newest last
    U64   lastProduced = 0;
    F64   lastArrival = -1.0;
    // per log window
    F64   winGap = 0.0;         // longest time with no new frames (viewer-side, tick resolution)
    F64   winBurst = 0.0;       // most audio that arrived between two ticks (s)
    F64   winStall = 0.0;       // deepest stall in the window (s)

    void clear()
    {
        anchored = false;
        buckets.clear();
        bucketStart = -1.0;
        bucketMax = 0.0;
    }

    void update(F64 now, U64 produced, U32 ep, bool playing, U32 rate, F64 drift)
    {
        if (!rate) return;
        if (!playing || !anchored || ep != epoch || now < lastT || produced < f0 || produced < lastProduced)
        {
            // (Re)anchor: paused / stopped, a flush, a new ring. The buckets are drawdowns, not
            // levels, so they stay valid across anchors.
            anchored = playing;
            t0 = lastT = now;
            f0 = lastProduced = produced;
            epoch = ep;
            peak = 0.0;
            lastArrival = now;
            if (bucketStart < 0.0) bucketStart = now;
            return;
        }
        const F64 dt = now - lastT;
        lastT = now;
        const F64 dev = (F64)(produced - f0) / (F64)rate - (now - t0) * (1.0 + drift);
        peak = std::max(dev, peak - kJitterLeak * dt);
        const F64 dd = peak - dev;
        bucketMax = std::max(bucketMax, dd);
        winStall = std::max(winStall, dd);
        if (now - bucketStart >= 1.0)
        {
            buckets.push_back((F32)bucketMax);
            while (buckets.size() > kJitterWindowSecs) buckets.pop_front();
            bucketMax = 0.0;
            bucketStart = now;
        }
        winGap = std::max(winGap, now - lastArrival);
        if (produced > lastProduced)
        {
            winBurst = std::max(winBurst, (F64)(produced - lastProduced) / (F64)rate);
            lastArrival = now;
            lastProduced = produced;
        }
    }

    // The deepest stall in the window (s).
    F64 jitter() const
    {
        F64 j = bucketMax;
        for (F32 b : buckets) j = std::max(j, (F64)b);
        return j;
    }
};

struct Active
{
    U32  src = 0;
    LLAudioEngine::EStreamRole role = LLAudioEngine::STREAM_MONO;
    F32  fade = 0.f;            // join / leave ramp 0..1
    bool leaving = false;
    F64  silentSince = -1.0;    // faded out (leaving) since; kept alive kKeepAliveSecs
    LLUUID key;                 // acoustics key
    LLAudioEngine::StreamSourceParams params;
};

struct State
{
    LLAudioEngine* engine = nullptr;
    LLPluginAudioRingReader* reader = nullptr;
    U64  lastConsumed = 0;
    U32  rate = 0;
    bool open = false;
    U32  bed = 0;
    F32  bedLevel = -1.f;       // < 0: snap on the next update
    std::map<LLUUID, Active> speakers;
    Resampler rs;
    Biquad sub[2];
    std::vector<S16> raw, stereo, mono, left, right, subPcm;
    std::vector<F32> out;
    SLParcelSpeakerAudio::EMode mode = SLParcelSpeakerAudio::MODE_OFF;
    F64  lastTick = -1.0;
    F64  nextLog = 0.0;
    F64  nextCaptureRestart = 0.0;
    F64  playingSince = -1.0;
    std::string seekKey;
    // jitter buffer
    ArrivalJitter jitter;
    std::string jitterUrl;      // the stream the jitter history belongs to
    F64  ringTarget = kRingFloorSecs;   // s, adaptive (see kRingFloorSecs)
    F64  aboveSince = -1.0;     // the ring has stayed above target + kSkipExcessSecs since
    U64  skips = 0, skippedFrames = 0, holds = 0;
    bool holding = false;       // a (re)start is waiting for the ring to reach its target
    // per log window
    F64  winFillMin = 1e30, winFillMax = 0.0, winTickGap = 0.0, winTargetMax = 0.0;
    U32  winDry = 0;            // ticks the feeder wanted audio the ring did not have
    // stats (session)
    U32  peakSpeakers = 0;
    U32  capLogInRange = 0, capLogPlaying = 0;   // last "N in range, M playing" logged
    U64  discontinuities = 0;
    U32  traced = 0, acAnswered = 0;
    bool hooked = false;
};

State& st()
{
    static State s;
    return s;
}

LLAudioEngine::EStreamRole roleOf(SLParcelSpeakers::ERole r)
{
    switch (r)
    {
    case SLParcelSpeakers::ROLE_LEFT:  return LLAudioEngine::STREAM_LEFT;
    case SLParcelSpeakers::ROLE_RIGHT: return LLAudioEngine::STREAM_RIGHT;
    case SLParcelSpeakers::ROLE_SUB:   return LLAudioEngine::STREAM_SUB;
    default:                           return LLAudioEngine::STREAM_MONO;
    }
}

// The level at the listener the ranking uses (OpenAL's inverse distance clamped + the soft edge).
F32 levelAt(F32 d, F32 range)
{
    const F32 ref = refDistance(range);
    const F32 dc = llclamp(d, ref, std::max(range, ref));
    return (ref / dc) * LLAudioEngine::softEdge(d, range);
}

LLVector3d earGlobal()
{
    static LLCachedControl<S32> s_ear(gSavedSettings, "MediaSoundsEarLocation", 0);
    return s_ear == 1 ? gAgent.getPositionGlobal() : gAgentCamera.getCameraPositionGlobal();
}

void destroyAll(State& s)
{
    if (s.engine && s.engine == gAudiop)
    {
        for (auto& kv : s.speakers) if (kv.second.src) gAudiop->streamDestroySource(kv.second.src);
        if (s.bed) gAudiop->streamDestroySource(s.bed);
        if (s.open) gAudiop->streamClose();
    }
    s.speakers.clear();
    s.bed = 0;
    s.open = false;
    s.bedLevel = -1.f;
    s.playingSince = -1.0;
}

void resetDsp(State& s)
{
    s.rs.reset();
    for (Biquad& b : s.sub) b.lowpass(120.0, s.rate ? (F64)s.rate : 48000.0);
}

// ---- feeding: keep the OpenAL queue at its target ----
void feed(State& s, LLPluginAudioRingReader* reader)
{
    static LLCachedControl<F32> s_buffer(gSavedSettings, "ParcelAudioBufferSeconds", 1.2f);
    const F64 target = llclamp((F64)s_buffer, kPrebufferSecs, 2.5);
    const U32 chunk = s.rate / 20;   // 50 ms
    const U32 ring_target = (U32)(s.ringTarget * s.rate);
    s.out.resize((size_t)chunk * 2);
    s.stereo.resize((size_t)chunk * 2);
    s.mono.resize(chunk);
    s.left.resize(chunk);
    s.right.resize(chunk);
    s.subPcm.resize(chunk);

    F64 queued = gAudiop->streamQueuedSeconds();
    for (U32 n = 0; n < kMaxChunksPerTick && queued < target; ++n)
    {
        const F64 ratio = reader->trimRatio(ring_target);
        const U32 need = s.rs.need(chunk, ratio);
        if (need)
        {
            // Top up only from what the ring holds: a short ring here is normal (the plugin
            // delivers in its own blocks); a real starvation shows as an OpenAL underrun.
            if (reader->available() < need)
            {
                ++s.winDry;
                break;
            }
            s.raw.resize((size_t)need * 2);
            LLPluginAudioRingReader::ReadInfo info;
            if (!reader->readExact(s.raw.data(), need, &info))
                break;
            if (info.discontinuity)
            {
                // A seek / flush in the plugin: drop what is queued and start clean with this audio.
                gAudiop->streamFlush();
                resetDsp(s);
                ++s.discontinuities;
                queued = 0.0;
                LL_INFOS("ParcelAudio") << "stream discontinuity (seek / flush): queue dropped, pre-buffering again" << LL_ENDL;
                s.rs.push(s.raw.data(), need);
                continue;   // (need() is measured again against the fresh history)
            }
            s.rs.push(s.raw.data(), need);
        }
        s.rs.produce(s.out.data(), chunk, ratio);
        for (U32 i = 0; i < chunk; ++i)
        {
            const F32 L = s.out[i * 2], R = s.out[i * 2 + 1];
            const F32 M = 0.5f * (L + R);
            s.stereo[i * 2] = toS16(L);
            s.stereo[i * 2 + 1] = toS16(R);
            s.mono[i] = toS16(M);
            s.left[i] = toS16(L);
            s.right[i] = toS16(R);
            s.subPcm[i] = toS16((F32)s.sub[1].run(s.sub[0].run(M)));
        }
        const S16* roles[LLAudioEngine::STREAM_ROLE_COUNT] = { s.stereo.data(), s.mono.data(), s.left.data(), s.right.data(),
                                                              s.subPcm.data() };
        if (!gAudiop->streamPush(roles, chunk))
            break;
        queued += (F64)chunk / s.rate;
    }
}

// ---- the speakers and the bed ----
void place(State& s, F32 dt, LLStreamingAudio_MediaPlugins* sa)
{
    static LLCachedControl<bool> s_on(gSavedSettings, "ParcelAudioSpeakers", false);
    static LLCachedControl<U32> s_max(gSavedSettings, "ParcelAudioMaxSpeakers", 16);
    static LLCachedControl<S32> s_outside(gSavedSettings, "ParcelAudioSpeakersOutside", 0);
    static LLCachedControl<F32> s_music(gSavedSettings, "AudioLevelMusic", 1.f);
    static LLCachedControl<bool> s_mute_music(gSavedSettings, "MuteMusic", false);
    static LLCachedControl<bool> s_diffuse(gSavedSettings, "ParcelAudioDiffuseReverb", false);
    static LLCachedControl<bool> s_doppler(gSavedSettings, "ParcelAudioSpeakerDoppler", false);

    // The stream's level as the plugin got it, minus master and mute (the listener gain).
    const F32 base = (s_mute_music ? 0.f : llclamp((F32)s_music, 0.f, 1.f)) * llclamp(LLViewerAudio::getInstance()->getFadeVolume(), 0.f, 1.f);

    const std::vector<SLParcelSpeakers::Speaker>& list = SLParcelSpeakers::speakers();
    const bool use = s_on && !list.empty();
    const LLVector3d ear = earGlobal();

    // Rank the audible ones.
    struct Cand { const SLParcelSpeakers::Speaker* sp; F32 score; F32 dist; };
    std::vector<Cand> cands;
    if (use)
    {
        for (const SLParcelSpeakers::Speaker& sp : list)
        {
            const F32 d = (F32)(sp.posGlobal - ear).length();
            const F32 lvl = levelAt(d, sp.tag.range);
            if (lvl <= 0.f) continue;
            F32 score = std::max(sp.tag.vol, 0.01f) * lvl;
            auto it = s.speakers.find(sp.id);
            if (it != s.speakers.end() && !it->second.leaving) score *= kKeepBonus;
            cands.push_back({ &sp, score, d });
        }
        std::sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) { return a.score > b.score; });
        const size_t cap = std::min<size_t>(llclamp((U32)s_max, 1u, kMaxSpeakers), kMaxSpeakers);
        // Speakers in range beyond the cap are silent: say so once per change, so a builder
        // knows more speakers will not be heard (raise ParcelAudioMaxSpeakers, up to 32).
        const U32 in_range_n = (U32)cands.size(), playing_n = (U32)std::min(cands.size(), cap);
        if (in_range_n != s.capLogInRange || playing_n != s.capLogPlaying)
        {
            if (in_range_n > playing_n)
                LL_INFOS("ParcelAudio") << in_range_n << " speakers in range, " << playing_n << " playing (cap " << cap
                                        << ", ParcelAudioMaxSpeakers): the quietest at your position are skipped" << LL_ENDL;
            else if (s.capLogInRange > s.capLogPlaying)
                LL_INFOS("ParcelAudio") << in_range_n << " speakers in range, all playing (cap " << cap << ")" << LL_ENDL;
            s.capLogInRange = in_range_n;
            s.capLogPlaying = playing_n;
        }
        if (cands.size() > cap) cands.resize(cap);
    }
    const bool in_range = !cands.empty();

    // Mode.
    SLParcelSpeakerAudio::EMode mode = SLParcelSpeakerAudio::MODE_STEREO;
    F32 bed_target = 1.f;
    if (use)
    {
        bool ambient = SLParcelSpeakers::outsidePolicy() == SLParcelSpeakers::OUTSIDE_AMBIENT;
        if (s_outside == 1) ambient = true;
        else if (s_outside == 2) ambient = false;
        mode = in_range ? SLParcelSpeakerAudio::MODE_SPEAKERS
                        : (ambient ? SLParcelSpeakerAudio::MODE_OUTSIDE_AMBIENT : SLParcelSpeakerAudio::MODE_OUTSIDE_SILENT);
        bed_target = in_range ? 0.f : (ambient ? kAmbientGain : 0.f);
    }
    if (mode != s.mode)
    {
        LL_INFOS("ParcelAudio") << "parcel stream output: " << SLParcelSpeakerAudio::modeName(mode) << " ("
                                << list.size() << " verified speaker(s), " << cands.size() << " in range)" << LL_ENDL;
        s.mode = mode;
    }

    // Leaving: everything not selected.
    for (auto& kv : s.speakers)
    {
        bool keep = false;
        for (const Cand& c : cands) keep |= c.sp->id == kv.first;
        if (!keep) kv.second.leaving = true;
    }

    const bool ac = gAudiop->acousticsActive();
    s.traced = 0;
    s.acAnswered = 0;
    for (const Cand& c : cands)
    {
        const SLParcelSpeakers::Speaker& sp = *c.sp;
        const LLAudioEngine::EStreamRole role = roleOf(sp.tag.role);
        auto it = s.speakers.find(sp.id);
        if (it != s.speakers.end() && it->second.role != role)
        {
            // The tag's ch= changed: a new source of the new role (the old one is dropped now).
            gAudiop->streamDestroySource(it->second.src);
            s.speakers.erase(it);
            it = s.speakers.end();
        }
        if (it == s.speakers.end())
        {
            Active a;
            a.role = role;
            a.src = gAudiop->streamCreateSource(role);
            if (!a.src) continue;
            sp.id.combine(kAcousticsNs, a.key);
            it = s.speakers.emplace(sp.id, a).first;
        }
        Active& a = it->second;
        a.leaving = false;
        a.silentSince = -1.0;

        LLAudioEngine::StreamSourceParams& p = a.params;
        p = LLAudioEngine::StreamSourceParams();
        const F32 range = sp.tag.range;
        const F32 edge = LLAudioEngine::softEdge(c.dist, range);
        p.relative = false;
        p.doppler = s_doppler;   // (off: no detuning / drift while you walk; position etc. unchanged)
        p.pos.setVec(sp.posGlobal);
        p.refDist = refDistance(range);
        p.maxDist = std::max(range, p.refDist);
        p.rolloff = gAudiop->getRolloffFactor();   // 1 (x5 under water, like the channels)
        if (sp.tag.hasDir && role != LLAudioEngine::STREAM_SUB)
        {
            p.cone = true;
            p.dir = LLVector3::x_axis * sp.rotation;
            p.dir.normVec();
            p.coneInner = sp.tag.dirDeg;
            p.coneOuter = std::min(360.f, sp.tag.dirDeg + kConeOuterExtra);
            p.coneOuterGain = kConeOuterGain;
        }
        p.gain = base * llclamp(sp.tag.vol, 0.f, SLParcelSpeakers::kMaxVol) * edge;   // (x the join ramp below)

        // Room acoustics: traced as an extra source under its own key.
        if (ac && gAgent.getRegion())
        {
            LLCleanAcoustics::traceExtraSource(a.key, gAgent.getPosAgentFromGlobal(sp.posGlobal));
            ++s.traced;
            LLCleanAcoustics::SourceParams sp_ac;
            if (LLCleanAcoustics::sourceAcoustics(a.key, sp_ac))
            {
                ++s.acAnswered;
                p.efx = true;
                p.diffuse = s_diffuse;   // (the sends keep their level over distance)
                p.directGain = sp_ac.direct_gain;
                p.directHF = sp_ac.direct_gain_hf;
                p.sendGain = sp_ac.send_gain;
                p.sendHF = sp_ac.send_gain_hf;
                p.zone = sp_ac.zone;
                p.zoneGain = sp_ac.zone_gain;
                p.zoneHF = sp_ac.zone_gain_hf;
                p.bassBoostDb = sp_ac.bass_db;   // #897 boundary bass
                p.directBoost = sp_ac.direct_boost;   // #683 early gain
                if (sp_ac.pan_weight > 0.f)
                {
                    // Diffraction: heard from the opening, at the true distance.
                    const LLVector3d rel = sp.posGlobal - ear;
                    const F64 dist = rel.length();
                    if (dist > 0.01)
                    {
                        LLVector3 dir((F32)(rel.mdV[VX] / dist), (F32)(rel.mdV[VY] / dist), (F32)(rel.mdV[VZ] / dist));
                        const F32 w = llclamp(sp_ac.pan_weight, 0.f, 1.f);
                        LLVector3 v = dir * (1.f - w) + sp_ac.pan_dir * w;
                        if (v.normVec() > 0.f)
                            p.pos.setVec(ear + LLVector3d(v) * dist);
                    }
                }
            }
        }
    }

    // Ramps, apply, and free the speakers that have been silent kKeepAliveSecs (or the oldest
    // silent ones beyond cap + kKeepAliveExtra). Until then a faded-out speaker keeps running on
    // the timeline at gain 0, so it comes back in step with the others.
    const F64 now = LLTimer::getTotalSeconds();
    const size_t keep_max = (size_t)std::min<U32>(llclamp((U32)s_max, 1u, kMaxSpeakers), kMaxSpeakers) + kKeepAliveExtra;
    const F32 step = dt / kSpeakerFadeSecs;
    for (auto it = s.speakers.begin(); it != s.speakers.end();)
    {
        Active& a = it->second;
        a.fade = a.leaving ? std::max(0.f, a.fade - step) : std::min(1.f, a.fade + step);
        if (a.leaving && a.fade <= 0.f)
        {
            if (a.silentSince < 0.0) a.silentSince = now;
            bool oldest = false;
            if (s.speakers.size() > keep_max)
            {
                oldest = true;
                for (const auto& kv : s.speakers)
                    if (kv.second.silentSince >= 0.0 && kv.second.silentSince < a.silentSince) { oldest = false; break; }
            }
            if (oldest || now - a.silentSince >= kKeepAliveSecs)
            {
                gAudiop->streamDestroySource(a.src);
                it = s.speakers.erase(it);
                continue;
            }
        }
        LLAudioEngine::StreamSourceParams p = a.params;
        p.gain *= a.fade;
        gAudiop->streamSetSource(a.src, p);
        ++it;
    }
    s.peakSpeakers = std::max(s.peakSpeakers, (U32)s.speakers.size());

    // The bed (also the group's clock: it always exists while the stream plays).
    if (s.bedLevel < 0.f) s.bedLevel = bed_target;
    const F32 bstep = dt / kBedFadeSecs;
    if (s.bedLevel < bed_target) s.bedLevel = std::min(bed_target, s.bedLevel + bstep);
    else if (s.bedLevel > bed_target) s.bedLevel = std::max(bed_target, s.bedLevel - bstep);
    LLAudioEngine::StreamSourceParams bp;
    bp.relative = true;
    bp.gain = base * s.bedLevel;
    gAudiop->streamSetSource(s.bed, bp);

    // t0= clips: seek once per stream and t0 to the synced time.
    if (use && sa)
    {
        const SLParcelSpeakers::Speaker* with_t0 = nullptr;
        F64 best = 1e30;
        for (const SLParcelSpeakers::Speaker& sp : list)
        {
            if (!sp.tag.hasT0) continue;
            const F64 d = (sp.posGlobal - ear).length();
            if (d < best) { best = d; with_t0 = &sp; }
        }
        if (with_t0)
        {
            const std::string key = sa->getURL() + llformat("|%lld", (long long)with_t0->tag.t0);
            if (key != s.seekKey && s.playingSince >= 0.0)
            {
                const F64 length = sa->getStreamDuration();
                const F64 now = LLTimer::getTotalSeconds();
                if (length > 1.0)
                {
                    // Heard (pre-buffer + ring + plugin restart) after the seek: aim that far ahead.
                    const F64 lead = kPrebufferSecs + s.ringTarget + 0.35;
                    const F64 elapsed = SLWeather::syncedSeconds() - (F64)with_t0->tag.t0 + lead;
                    const F64 at = elapsed <= 0.0 ? 0.0 : std::fmod(elapsed, length);
                    sa->seekStream(at);
                    s.seekKey = key;
                    LL_INFOS("ParcelAudio") << "t0 clip: seeking to " << llformat("%.1f", at) << " s of " << llformat("%.1f", length)
                                            << " s (t0 " << with_t0->tag.t0 << ")" << LL_ENDL;
                }
                else if (now - s.playingSince > 8.0)
                {
                    s.seekKey = key;
                    LL_INFOS("ParcelAudio") << "t0 given but the stream reports no length (live): not seeked" << LL_ENDL;
                }
            }
        }
    }
}

void logStats(State& s, LLPluginAudioRingReader* reader)
{
    static LLCachedControl<U32> s_max(gSavedSettings, "ParcelAudioMaxSpeakers", 16);
    const LLAudioEngine::StreamStats es = gAudiop->streamStats();
    const LLPluginAudioRingReader::Stats& rs = reader->stats();
    LL_INFOS("ParcelAudio") << "speakers: mode " << SLParcelSpeakerAudio::modeName(s.mode) << ", live " << s.speakers.size()
                            << " / cap " << std::min<U32>(llclamp((U32)s_max, 1u, kMaxSpeakers), kMaxSpeakers) << " (peak "
                            << s.peakSpeakers << "), verified " << SLParcelSpeakers::speakers().size() << ", bed "
                            << llformat("%.2f", std::max(s.bedLevel, 0.f)) << ", acoustics " << s.acAnswered << "/" << s.traced
                            << " | queue " << (S32)(es.queuedSeconds * 1000.0) << " ms (" << (es.playing ? "playing" : es.paused ? "paused" : "buffering")
                            << "), ring fill " << (S32)(rs.fillSeconds * 1000.0) << " ms, drift "
                            << (rs.driftValid ? llformat("%+.1f ppm", rs.driftPpm) : std::string("(measuring)")) << ", trim "
                            << llformat("%.5f", reader->trimRatio((U32)(s.ringTarget * s.rate)))
                            << " | ring underruns " << rs.underruns << ", AL underruns " << es.underruns << ", restarts " << es.restarts
                            << ", starts " << es.starts << ", late joins " << es.lateJoins << ", join fallbacks " << es.joinFallbacks
                            << ", flushes " << es.flushes << ", discontinuities " << s.discontinuities << ", plugin dropped "
                            << rs.producerDropped << ", push refused " << es.pushRejected << " | AL sources " << es.sources << " (peak "
                            << es.sourcesPeak << "), buffers " << es.buffersLive << " (peak " << es.buffersPeak << ", pooled "
                            << es.buffersPooled << "), chunks " << es.chunks << (es.syncStart ? "" : ", no PlayAtTime (joins restart the group)")
                            << " | sync: max offset " << llformat("%.1f", es.syncOffsetMaxMs) << " ms, speaker spread "
                            << llformat("%.1f", es.syncSpreadMaxMs) << " ms, join error " << llformat("%.1f", es.syncJoinErrMaxMs)
                            << " ms, resyncs " << es.syncResyncs << ", deferred (moving) " << es.syncDeferred << ", checks "
                            << es.syncChecks << LL_ENDL;

    // The jitter buffer: is the producer steady (stall ~0, target at the floor) or does it stall
    // and burst (the target follows; ring min / max swing)? A long tick is the viewer's own hitch
    // (the OpenAL queue covers it), not the producer.
    ArrivalJitter& j = s.jitter;
    LL_INFOS("ParcelAudio") << "speakers jitter: ring target " << (S32)(s.ringTarget * 1000.0) << " ms (max "
                            << (S32)(s.winTargetMax * 1000.0) << "), deepest stall " << (S32)(j.winStall * 1000.0)
                            << " ms (window " << (S32)(j.jitter() * 1000.0) << " ms over " << j.buckets.size()
                            << " s), ring fill " << (S32)(std::min(s.winFillMin, 1e6) * 1000.0) << ".."
                            << (S32)(s.winFillMax * 1000.0) << " ms of "
                            << (S32)((F64)reader->capacityFrames() * 1000.0 / (F64)std::max<U32>(s.rate, 1))
                            << " | arrivals: longest gap " << (S32)(j.winGap * 1000.0) << " ms, largest per tick "
                            << (S32)(j.winBurst * 1000.0) << " ms | longest tick " << (S32)(s.winTickGap * 1000.0)
                            << " ms, dry ticks " << s.winDry << " | start holds " << s.holds << ", skips " << s.skips
                            << " (" << s.skippedFrames << " frames)" << LL_ENDL;
    j.winGap = j.winBurst = j.winStall = 0.0;
    s.winFillMin = 1e30;
    s.winFillMax = 0.0;
    s.winTickGap = 0.0;
    s.winTargetMax = 0.0;
    s.winDry = 0;
}

// The plugin's own view of its delivery (GStreamer: media_plugin_gstreamer10 AudioArrival).
void logDelivery(LLStreamingAudio_MediaPlugins* sa)
{
    const LLSD d = sa ? sa->takeCaptureDelivery() : LLSD();
    if (!d.isMap() || !d.has("reports")) return;
    std::string pts = "?";
    if (d.has("pts_min_ms") && d.has("pts_max_ms"))
        pts = llformat("%+.0f..%+.0f ms", d["pts_min_ms"].asReal(), d["pts_max_ms"].asReal());
    LL_INFOS("ParcelAudio") << "plugin delivery (" << d["reports"].asInteger() << " reports): " << d["arr_samples"].asInteger()
                            << " buffers (largest " << (S32)d["arr_max_buffer_frames"].asReal() << " frames), longest gap "
                            << (S32)d["arr_gap_ms"].asReal() << " ms, deepest stall " << (S32)d["arr_drawdown_ms"].asReal()
                            << " ms, largest burst " << (S32)d["arr_runup_ms"].asReal() << " ms, PTS - wall " << pts
                            << " (max rising above ~0 = the appsink is not pacing), no PTS " << d["pts_none"].asInteger()
                            << ", queue2 buffering min "
                            << (d.has("buffering_min") ? llformat("%d %%", (S32)d["buffering_min"].asInteger()) : std::string("(none posted)"))
                            << ", last " << (d.has("buffering_last") ? llformat("%d %%", (S32)d["buffering_last"].asInteger()) : std::string("-"))
                            << LL_ENDL;
}
} // namespace

// static
const char* SLParcelSpeakerAudio::modeName(EMode m)
{
    switch (m)
    {
    case MODE_OFF:             return "plugin output";
    case MODE_STEREO:          return "stereo (no speakers)";
    case MODE_SPEAKERS:        return "speakers";
    case MODE_OUTSIDE_AMBIENT: return "out of range: ambient bed";
    case MODE_OUTSIDE_SILENT:  return "out of range: silent";
    }
    return "?";
}

// static
SLParcelSpeakerAudio::EMode SLParcelSpeakerAudio::mode()
{
    return st().mode;
}

// static
void SLParcelSpeakerAudio::shutdown()
{
    State& s = st();
    destroyAll(s);
    s.reader = nullptr;
    s.engine = nullptr;
    s.mode = MODE_OFF;
}

// static
void SLParcelSpeakerAudio::update()
{
    State& s = st();
    if (!s.hooked)
    {
        // Speakers need the viewer to own the stream's audio: turning them on turns capture on
        // (applied below by restarting a playing stream). Capture stays on when they go off:
        // the stereo bed then plays exactly what the plugin did.
        s.hooked = true;
        if (LLControlVariable* c = gSavedSettings.getControl("ParcelAudioSpeakers"))
        {
            c->getSignal()->connect([](LLControlVariable*, const LLSD& v, const LLSD&)
            {
                if (v.asBoolean() && !gSavedSettings.getBOOL("ParcelAudioCapture"))
                {
                    LL_INFOS("ParcelAudio") << "ParcelAudioSpeakers on: turning ParcelAudioCapture on" << LL_ENDL;
                    gSavedSettings.setBOOL("ParcelAudioCapture", true);
                }
            });
        }
    }
    if (gAudiop != s.engine)
    {
        // A new (or no) engine: the old one's sources died with it.
        s.speakers.clear();
        s.bed = 0;
        s.open = false;
        s.reader = nullptr;
        s.bedLevel = -1.f;
        s.engine = gAudiop;
    }
    if (!gAudiop) return;

    const F64 now = LLTimer::getTotalSeconds();
    const F64 tick_gap = s.lastTick < 0.0 ? 0.0 : now - s.lastTick;
    const F32 dt = s.lastTick < 0.0 ? 0.f : (F32)llclamp(now - s.lastTick, 0.0, 0.25);
    s.lastTick = now;

    LLStreamingAudio_MediaPlugins* sa = dynamic_cast<LLStreamingAudio_MediaPlugins*>(gAudiop->getStreamingAudioImpl());
    if (!sa)
    {
        if (s.open) destroyAll(s);
        s.mode = MODE_OFF;
        return;
    }
    sa->setCaptureProbe(false);   // we drain the ring now

    // ParcelAudioCapture is read when the plugin is created: a change restarts a playing stream.
    // (captureRequested: a stream whose capture failed plays through the plugin, #960; that is not
    // a setting change.)
    static LLCachedControl<bool> s_capture(gSavedSettings, "ParcelAudioCapture", false);
    if (sa->captureRequested() != (bool)s_capture && !sa->getURL().empty() && sa->isPlaying() == 1 && now >= s.nextCaptureRestart)
    {
        s.nextCaptureRestart = now + 5.0;
        const std::string url = sa->getURL();
        LL_INFOS("ParcelAudio") << "ParcelAudioCapture " << (s_capture ? "on" : "off") << ": restarting the parcel stream" << LL_ENDL;
        destroyAll(s);
        s.reader = nullptr;
        gAudiop->stopInternetStream();
        gAudiop->startInternetStream(url);
        s.mode = MODE_OFF;
        return;
    }

    // The jitter history belongs to a stream (its network path): kept across plugin restarts.
    if (sa->getURL() != s.jitterUrl)
    {
        s.jitterUrl = sa->getURL();
        s.jitter.clear();
        s.ringTarget = kRingFloorSecs;
    }

    // The plugin is recreated on every stream stop / start: fetch the reader every tick.
    LLPluginAudioRingReader* reader = sa->getCaptureReader();
    if (!reader || reader->sampleRate() == 0 || reader->channels() != 2)
    {
        // No ring: nothing for us to play. (Capturing without one is LLStreamingAudio_MediaPlugins'
        // business: it falls back to the plugin's own output and warns, #960.)
        if (s.open) destroyAll(s);
        s.reader = nullptr;
        if (s.mode != MODE_OFF)
        {
            LL_INFOS("ParcelAudio") << "parcel stream output: " << modeName(MODE_OFF) << LL_ENDL;
            s.mode = MODE_OFF;
        }
        return;
    }
    if (reader != s.reader || reader->consumedFrames() < s.lastConsumed || reader->sampleRate() != s.rate)
    {
        // Another ring (a new plugin instance): start clean.
        destroyAll(s);
        s.reader = reader;
        s.rate = reader->sampleRate();
        s.seekKey.clear();
    }
    if (!s.open)
    {
        if (!gAudiop->streamOpen(s.rate))
        {
            static bool s_warned = false;
            if (!s_warned)
            {
                s_warned = true;
                LL_WARNS("ParcelAudio") << "the audio engine has no stream output: the captured parcel stream stays silent "
                                        << "(set ParcelAudioCapture off for the plugin's own output)" << LL_ENDL;
            }
            return;
        }
        s.open = true;
        resetDsp(s);
        s.bed = gAudiop->streamCreateSource(LLAudioEngine::STREAM_STEREO);
        s.bedLevel = -1.f;
        s.nextLog = now + kLogInterval;
    }

    // Producer state: pause / resume the group with it.
    const U32 pstate = reader->producerState();
    gAudiop->streamPause(pstate == LLPluginAudioRing::STATE_PAUSED);
    if (pstate == LLPluginAudioRing::STATE_PLAYING)
    {
        if (s.playingSince < 0.0) s.playingSince = now;
    }
    else if (pstate != LLPluginAudioRing::STATE_PAUSED)
    {
        s.playingSince = -1.0;
    }

    // The consumer clock is the output device's.
    const F64 dev = gAudiop->streamDeviceSeconds();
    reader->updateClock(dev >= 0.0 ? dev : now);

    // The jitter buffer's target: the deepest producer stall seen, plus a margin, less what the
    // OpenAL queue holds; within [floor, capacity - 1 s].
    static LLCachedControl<F32> s_buffer(gSavedSettings, "ParcelAudioBufferSeconds", 1.2f);
    const F64 al_target = llclamp((F64)s_buffer, kPrebufferSecs, 2.5);
    s.jitter.update(now, reader->producedFrames(), reader->producerEpoch(), pstate == LLPluginAudioRing::STATE_PLAYING,
                    s.rate, reader->driftValid() ? reader->driftPpm() * 1e-6 : 0.0);
    const F64 cap_secs = (F64)reader->capacityFrames() / (F64)s.rate;
    s.ringTarget = llclamp(s.jitter.jitter() + kJitterMarginSecs - al_target, kRingFloorSecs,
                           std::max(kRingFloorSecs, cap_secs - 1.0));
    s.winTargetMax = std::max(s.winTargetMax, s.ringTarget);
    s.winTickGap = std::max(s.winTickGap, tick_gap);

    feed(s, reader);
    place(s, dt, sa);

    const LLAudioEngine::StreamStats es = gAudiop->streamStats();
    const F64 fill = (F64)reader->available() / (F64)s.rate;
    s.winFillMin = std::min(s.winFillMin, fill);
    s.winFillMax = std::max(s.winFillMax, fill);

    // A (re)start waits until the ring also holds its target: starting on the OpenAL pre-buffer
    // alone left no cushion for the next stall, so a bursty stream underran again and again.
    // (Only while the producer plays: a stream that ended starts with what is there.)
    // Applied whether or not the group plays now: streamUpdate uses the pre-buffer only once it is
    // not playing, and an underrun found inside this very streamUpdate restarts the group there
    // (the burst that just arrived must not start it with an empty ring).
    F64 prebuffer = kPrebufferSecs;
    const bool wait_ring = pstate == LLPluginAudioRing::STATE_PLAYING && fill + 0.05 < s.ringTarget;
    if (wait_ring)
        prebuffer = 1e9;    // (a desync restart still starts on its own small minimum)
    const bool holding = wait_ring && !es.playing && !es.paused;
    if (holding && !s.holding) ++s.holds;
    s.holding = holding;

    // A backlog that has stayed well above the target for kSkipHoldSecs was never needed (the
    // producer did not stall that deep meanwhile): skip it once instead of carrying the latency
    // (the +-0.2 % trim would take minutes). Unlike a fixed target, this target already covers
    // the stalls this stream has.
    if (es.playing && pstate == LLPluginAudioRing::STATE_PLAYING && fill > s.ringTarget + kSkipExcessSecs)
    {
        if (s.aboveSince < 0.0) s.aboveSince = now;
        else if (now - s.aboveSince >= kSkipHoldSecs)
        {
            const U32 skipped = reader->skipTo((U32)(s.ringTarget * s.rate));
            ++s.skips;
            s.skippedFrames += skipped;
            s.aboveSince = -1.0;
            LL_INFOS("ParcelAudio") << "ring backlog unused for " << (S32)kSkipHoldSecs << " s: skipped " << skipped
                                    << " frames down to the " << (S32)(s.ringTarget * 1000.0) << " ms target" << LL_ENDL;
        }
    }
    else
    {
        s.aboveSince = -1.0;
    }

    static LLCachedControl<F32> s_sync_tol(gSavedSettings, "ParcelAudioSyncToleranceMs", 10.f);
    gAudiop->streamSetSyncTolerance(s_sync_tol);
    gAudiop->streamUpdate(prebuffer);
    s.lastConsumed = reader->consumedFrames();

    if (now >= s.nextLog)
    {
        s.nextLog = now + kLogInterval;
        logStats(s, reader);
        logDelivery(sa);
    }
}
