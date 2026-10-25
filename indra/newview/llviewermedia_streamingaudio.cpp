/**
 * @file llviewermedia_streamingaudio.h
 * @author Tofu Linden, Sam Kolb
 * @brief LLStreamingAudio_MediaPlugins implementation - an implementation of the streaming audio interface which is implemented as a client of the media plugin API.
 *
 * $LicenseInfo:firstyear=2009&license=viewerlgpl$
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
#include "llviewerprecompiledheaders.h"
#include "linden_common.h"
#include "llpluginclassmedia.h"
#include "llpluginclassmediaowner.h"
#include "llviewermedia.h"

#include "llviewermedia_streamingaudio.h"

#include "llmimetypes.h"
#include "lldir.h"
#include "llpluginaudioringreader.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "slmediaurlpolicy.h"

#include <algorithm>
#include <cmath>
#include <vector>

LLStreamingAudio_MediaPlugins::LLStreamingAudio_MediaPlugins() :
    mMediaPlugin(NULL),
    mGain(1.0)
{
    // nothing interesting to do?
    // we will lazily create a media plugin at play-time, if none exists.
}

LLStreamingAudio_MediaPlugins::~LLStreamingAudio_MediaPlugins()
{
    delete mMediaPlugin;
    mMediaPlugin = NULL;
}

void LLStreamingAudio_MediaPlugins::clearMetadata()
{
    if (mMetadata.isDefined() && mMetadata.size() > 0)
    {
        mMetadata = LLSD();
        mMetadataUpdateSignal(mMetadata);
    }
}

// Tags arrive as title / artist / now_playing / genre / url. Radio streams (ICY) put the
// current track as "Artist - Title" in now_playing and the station name in title; files and
// ID3 streams give artist and title directly.
void LLStreamingAudio_MediaPlugins::onStreamMetadata(const LLSD& tags)
{
    const std::string now_playing = tags["now_playing"].asString();
    const std::string title = tags["title"].asString();
    const std::string artist = tags["artist"].asString();

    LLSD meta = LLSD::emptyMap();
    if (!now_playing.empty())
    {
        const size_t sep = now_playing.find(" - ");
        if (sep != std::string::npos)
        {
            meta["ARTIST"] = now_playing.substr(0, sep);
            meta["TITLE"] = now_playing.substr(sep + 3);
        }
        else
        {
            meta["TITLE"] = now_playing;
        }
        if (!title.empty() && title != now_playing)
        {
            meta["TRSN"] = title;       // station name
        }
    }
    else
    {
        if (!artist.empty()) meta["ARTIST"] = artist;
        if (!title.empty())  meta["TITLE"] = title;
    }
    if (!tags["url"].asString().empty())
    {
        meta["URL"] = tags["url"].asString();
    }

    mMetadata = meta;
    mMetadataUpdateSignal(mMetadata);
}

void LLStreamingAudio_MediaPlugins::start(const std::string& url)
{
    clearMetadata();    // the previous stream's tags must not stay on a new one
    if (mCaptureFallback && !url.empty())
    {
        // #960: the capture fallback covers only the stream it happened on; a new start tries again.
        if (mMediaPlugin)
        {
            mMediaPlugin->stop();
            delete mMediaPlugin;
            mMediaPlugin = nullptr;
        }
    }
    mCaptureFallback = false;
    if (!mMediaPlugin) // lazy-init the underlying media plugin
    {
        mMediaPlugin = initializeMedia("audio/mpeg"); // assumes that whatever media implementation supports mp3 also supports vorbis.
        LL_INFOS() << "streaming audio mMediaPlugin is now " << mMediaPlugin << LL_ENDL;
    }

    if(!mMediaPlugin)
        return;

    if (!url.empty())
    {
        LL_INFOS() << "Starting internet stream: " << url << LL_ENDL;

        mURL = url; // keep original url here for comparison purposes
        mPlayURL.clear();
        std::string snt_url = url;
        LLStringUtil::trim(snt_url);
        size_t pos = snt_url.find(' ');
        if (pos != std::string::npos)
        {
            // fmod permited having names after the url and people were using it.
            // People label their streams this way, ignore the 'label'.
            snt_url = snt_url.substr(0, pos);
        }
        const U32 generation = ++mStartGeneration;
        if (!LLViewerMedia::isAllowedStreamURL(snt_url))
        {
            LL_WARNS() << "Not playing a stream with a non-network url" << LL_ENDL;
            return;
        }
        // The stream's host must be a public address (resolved off the main thread).
        std::weak_ptr<int> alive = mAliveToken;
        const SLMediaURLPolicy::Verdict verdict = SLMediaURLPolicy::check(snt_url, "Parcel music",
            [this, alive, generation, snt_url](bool allowed)
            {
                // A later start() / stop() supersedes this answer.
                if (!allowed || alive.expired() || generation != mStartGeneration)
                {
                    return;
                }
                playChecked(snt_url);
            });
        if (verdict == SLMediaURLPolicy::Verdict::ALLOWED)
        {
            playChecked(snt_url);
        }
    }
    else
    {
        LL_INFOS() << "setting stream to NULL"<< LL_ENDL;
        ++mStartGeneration;
        mURL.clear();
        mMediaPlugin->stop();
        delete mMediaPlugin;
        mMediaPlugin = nullptr;
    }
}

void LLStreamingAudio_MediaPlugins::playChecked(const std::string& url)
{
    if (!mMediaPlugin)
    {
        return;
    }
    mPlayURL = url;
    mMediaPlugin->loadURI(url);
    mMediaPlugin->start();
    LL_INFOS() << "Playing stream..." << LL_ENDL;
}

void LLStreamingAudio_MediaPlugins::stop()
{
    ++mStartGeneration;
    clearMetadata();
    LL_INFOS() << "Stopping internet stream." << LL_ENDL;
    if(mMediaPlugin)
    {
        mMediaPlugin->stop();
        delete mMediaPlugin;
        mMediaPlugin = nullptr;
    }

    mURL.clear();
    mPlayURL.clear();
    mCaptureFallback = false;
}

void LLStreamingAudio_MediaPlugins::pause(int pause)
{
    if(!mMediaPlugin)
        return;

    if(pause)
    {
        LL_INFOS() << "Pausing internet stream." << LL_ENDL;
        mMediaPlugin->pause();
    }
    else
    {
        LL_INFOS() << "Unpausing internet stream." << LL_ENDL;
        mMediaPlugin->start();
    }
}

void LLStreamingAudio_MediaPlugins::update()
{
    if (mMediaPlugin)
        mMediaPlugin->idle();
    if (mCapture)
        checkCaptureDelivery();
    if (mCapture && mCaptureProbe)
        updateCaptureProbe();
}

void LLStreamingAudio_MediaPlugins::checkCaptureDelivery()
{
    if (!mMediaPlugin || mCaptureDelivered)
    {
        return;
    }
    const std::string& unavailable = mMediaPlugin->getAudioCaptureUnavailable();
    if (!unavailable.empty())
    {
        fallBackFromCapture(unavailable);
        return;
    }
    // The clock starts when the plugin reports playing (not while it connects / buffers) and
    // restarts after a pause.
    if (mMediaPlugin->getStatus() != LLPluginClassMediaOwner::MEDIA_PLAYING)
    {
        mCapturePlayingSince = -1.0;
        return;
    }
    LLPluginAudioRingReader* reader = mMediaPlugin->getAudioCaptureReader();
    if (reader && reader->producedFrames() > 0)
    {
        mCaptureDelivered = true;   // capture works for this instance; later stalls are the jitter buffer's business
        return;
    }
    const F64 now = LLTimer::getTotalSeconds();
    if (mCapturePlayingSince < 0.0)
    {
        mCapturePlayingSince = now;
        return;
    }
    if (now - mCapturePlayingSince < kCaptureDeliverySecs)
    {
        return;
    }
    fallBackFromCapture(reader ? llformat("no audio reached the ring %.0f s after the plugin started playing", kCaptureDeliverySecs)
                               : llformat("the audio ring was not attached %.0f s after the plugin started playing", kCaptureDeliverySecs));
}

void LLStreamingAudio_MediaPlugins::fallBackFromCapture(const std::string& reason)
{
    LL_WARNS("ParcelAudio") << "Parcel stream: audio capture failed (" << reason << "): the media plugin plays the stream "
                            << "itself (no in-world speakers) until the next stream start" << LL_ENDL;
    // The plugin's player was built for capture (libVLC: its audio output forced to memory), so
    // replay the stream on a fresh plugin that never asked for it.
    if (mMediaPlugin)
    {
        mMediaPlugin->stop();
        delete mMediaPlugin;
        mMediaPlugin = nullptr;
    }
    mMediaPlugin = initializeMedia("audio/mpeg", false);
    mCaptureFallback = true;
    if (!mMediaPlugin)
    {
        return;
    }
    mMediaPlugin->setVolume(llclamp(mGain, 0.f, 1.f));
    if (!mPlayURL.empty())
    {
        playChecked(mPlayURL);
    }
}

LLPluginAudioRingReader* LLStreamingAudio_MediaPlugins::getCaptureReader() const
{
    return (mCapture && mMediaPlugin) ? mMediaPlugin->getAudioCaptureReader() : nullptr;
}

LLSD LLStreamingAudio_MediaPlugins::takeCaptureDelivery()
{
    return (mCapture && mMediaPlugin) ? mMediaPlugin->takeAudioCaptureDelivery() : LLSD();
}

// #892 phase A: no OpenAL consumer yet. Drain at the wall-clock rate (the consumer clock), so
// the drift estimate, underrun count and fill level mean what they will mean in phase B.
void LLStreamingAudio_MediaPlugins::updateCaptureProbe()
{
    LLPluginAudioRingReader* reader = getCaptureReader();
    const F64 now = LLTimer::getTotalSeconds();
    if (!reader || reader->sampleRate() == 0)
    {
        mProbeLastTime = -1.0;
        mProbePrimed = false;
        return;
    }
    reader->updateClock(now);
    if (mProbeLastTime < 0.0)
    {
        mProbeLastTime = now;
        mProbeNextLog = now + 10.0;
        mProbeOwed = 0.0;
        return;
    }
    const F64 dt = llclamp(now - mProbeLastTime, 0.0, 0.5);   // (a long hitch is not a reason to dump the ring)
    mProbeLastTime = now;
    mProbeOwed += dt * reader->sampleRate();

    // Keep ~250 ms buffered like a real feeder would, read in 20 ms chunks.
    const U32 chunk = reader->sampleRate() / 50;
    const U32 target = reader->sampleRate() / 4;
    static std::vector<S16> scratch;
    scratch.resize((size_t)chunk * reader->channels());
    // Pre-roll to the target like a real feeder; after an underrun, pre-roll again.
    if (!mProbePrimed)
    {
        mProbePrimed = reader->available() >= target;
        mProbeOwed = 0.0;
    }
    while (mProbePrimed && mProbeOwed >= chunk)
    {
        if (!reader->readExact(scratch.data(), chunk))   // (counts the underrun)
        {
            mProbePrimed = false;
            mProbeOwed = 0.0;
            break;
        }
        mProbeOwed -= chunk;
        for (S16 v : scratch)
            mProbePeak = std::max(mProbePeak, std::fabs((F32)v) / 32768.f);
    }

    if (now >= mProbeNextLog)
    {
        mProbeNextLog = now + 10.0;
        const LLPluginAudioRingReader::Stats& st = reader->stats();
        const S64 pts = reader->streamTimeUs();
        LL_INFOS("ParcelAudio") << "capture probe: fill " << (S32)(st.fillSeconds * 1000.0) << " ms, read "
                                << st.framesRead << " frames, underruns " << st.underruns << ", plugin dropped "
                                << st.producerDropped << ", discontinuities " << st.discontinuities << ", drift "
                                << (st.driftValid ? llformat("%+.1f ppm", st.driftPpm) : std::string("(measuring)"))
                                << ", trim " << llformat("%.5f", reader->trimRatio(target)) << ", stream time "
                                << (pts >= 0 ? llformat("%.3f s", (F64)pts / 1e6) : std::string("?"))
                                << ", peak " << llformat("%.3f", mProbePeak) << LL_ENDL;
        mProbePeak = 0.f;
    }
}

int LLStreamingAudio_MediaPlugins::isPlaying()
{
    if (!mMediaPlugin)
        return 0; // stopped

    LLPluginClassMediaOwner::EMediaStatus status =
        mMediaPlugin->getStatus();

    switch (status)
    {
    case LLPluginClassMediaOwner::MEDIA_LOADING: // but not MEDIA_LOADED
    case LLPluginClassMediaOwner::MEDIA_PLAYING:
        return 1; // Active and playing
    case LLPluginClassMediaOwner::MEDIA_PAUSED:
        return 2; // paused
    default:
        return 0; // stopped
    }
}

void LLStreamingAudio_MediaPlugins::setGain(F32 vol)
{
    mGain = vol;

    if(!mMediaPlugin)
        return;

    vol = llclamp(vol, 0.f, 1.f);
    mMediaPlugin->setVolume(vol);
}

F32 LLStreamingAudio_MediaPlugins::getGain()
{
    return mGain;
}

F64 LLStreamingAudio_MediaPlugins::getStreamDuration() const
{
    return mMediaPlugin ? std::max(mMediaPlugin->getDuration(), 0.0) : 0.0;
}

bool LLStreamingAudio_MediaPlugins::seekStream(F64 seconds)
{
    if (!mMediaPlugin) return false;
    mMediaPlugin->seek((float)std::max(seconds, 0.0));
    return true;
}

std::string LLStreamingAudio_MediaPlugins::getURL()
{
    return mURL;
}

LLPluginClassMedia* LLStreamingAudio_MediaPlugins::initializeMedia(const std::string& media_type, bool allow_capture)
{
    LLPluginClassMediaOwner* owner = NULL;
    S32 default_size = 1; // audio-only - be minimal, doesn't matter
    F64 default_zoom = 1.0;
    LLPluginClassMedia* media_source = LLViewerMediaImpl::newSourceFromMediaType(media_type, owner, default_size, default_size, default_zoom);

    mCapture = false;
    mCaptureDelivered = false;
    mCapturePlayingSince = -1.0;
    mProbeLastTime = -1.0;
    if (media_source)
    {
        media_source->setLoop(false); // audio streams are not expected to loop
        media_source->setStreamMetadataCallback([this](const LLSD& tags) { onStreamMetadata(tags); });

        // #892: only this (parcel stream) instance ever captures; parcel media keeps its output.
        // Must precede loadURI: the request is queued ahead of load_uri.
        static LLCachedControl<bool> capture(gSavedSettings, "ParcelAudioCapture", false);
        static LLCachedControl<bool> capture_test_fail(gSavedSettings, "ParcelAudioCaptureTestFail", false);
        if (capture && allow_capture)
        {
            media_source->setAudioCapture(true, capture_test_fail);
            mCapture = true;
            LL_INFOS("ParcelAudio") << "Parcel stream: audio capture requested (the plugin will not play it itself)"
                                    << (capture_test_fail ? " [ParcelAudioCaptureTestFail: the plugin discards it]" : "") << LL_ENDL;
        }
    }

    return media_source;
}

