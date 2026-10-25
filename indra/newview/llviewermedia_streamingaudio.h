/**
 * @file llviewermedia_streamingaudio.h
 * @author Tofu Linden
 * @brief Definition of LLStreamingAudio_MediaPlugins implementation - an implementation of the streaming audio interface which is implemented as a client of the media plugins API.
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

#ifndef LL_VIEWERMEDIA_STREAMINGAUDIO_H
#define LL_VIEWERMEDIA_STREAMINGAUDIO_H


#include "stdtypes.h" // from llcommon

#include "llstreamingaudio.h"

#include <memory>

class LLPluginClassMedia;
class LLPluginAudioRingReader;

class LLStreamingAudio_MediaPlugins final : public LLStreamingAudioInterface
{
 public:
    LLStreamingAudio_MediaPlugins();
    /*virtual*/ ~LLStreamingAudio_MediaPlugins();

    /*virtual*/ void start(const std::string& url) override;
    /*virtual*/ void stop() override;
    /*virtual*/ void pause(int pause) override;
    /*virtual*/ void update() override;
    /*virtual*/ int isPlaying() override;
    /*virtual*/ void setGain(F32 vol) override;
    /*virtual*/ F32 getGain() override;
    /*virtual*/ std::string getURL() override;

    bool supportsAdjustableBufferSizes() override {return false;}
    void setBufferSizes(U32 streambuffertime, U32 decodebuffertime) override {};

    // #594: internet radio tags from the media plugin ("stream_metadata" message, libvlc).
    virtual bool supportsMetaData() override { return true; }
    LLSD getMetadata() const override { return mMetadata; }
    virtual bool supportsWaveData() override { return false; }
    virtual bool getWaveData(float* arr, S32 count, S32 stride = 1) override { return false; }

    // #892 audio capture (ParcelAudioCapture, read when the plugin is created, i.e. at the next
    // stream start). On: the plugin plays nothing itself; its PCM (48 kHz stereo S16) lands in
    // a shared ring and this is its consumer. Null while capture is off or not set up yet.
    // SLParcelSpeakerAudio (the OpenAL stream speakers / stereo bed) reads from it on the main thread:
    //   auto* sa = dynamic_cast<LLStreamingAudio_MediaPlugins*>(gAudiop->getStreamingAudioImpl());
    //   if (LLPluginAudioRingReader* r = sa ? sa->getCaptureReader() : nullptr) { ... }
    // and applies getGain() itself (the plugin captures at unity).
    LLPluginAudioRingReader* getCaptureReader() const;
    // The plugin's delivery diagnostics since the last call (LLPluginClassMedia::takeAudioCaptureDelivery).
    LLSD takeCaptureDelivery();
    bool isCapturing() const { return mCapture; }
    // #960: what ParcelAudioCapture asked of this stream: also true after capture failed and fell
    // back to the plugin's own output (isCapturing() false), so a settings watcher does not take
    // the fallback for a setting change and restart into the same failure.
    bool captureRequested() const { return mCapture || mCaptureFallback; }
    // While capturing with nobody else draining, update() drains the ring at real time and logs
    // fill / drift / underruns / level every 10 s (the stream is silent in that state).
    // SLParcelSpeakerAudio turns the probe off every tick while it consumes the ring.
    void setCaptureProbe(bool on) { mCaptureProbe = on; }
    // #892 t0= clips (slparcelspeakeraudio.cpp): the stream's length in seconds as the plugin
    // reports it (0: unknown or a live stream), and a seek (false: no plugin).
    F64  getStreamDuration() const;
    bool seekStream(F64 seconds);

private:
    void updateCaptureProbe();
    // #960: capture must never leave the parcel stream silent. Falls back (below) when the plugin
    // says capture cannot work, or no frame reached the ring kCaptureDeliverySecs after it plays.
    void checkCaptureDelivery();
    // Recreates the plugin without capture and replays the stream: the plugin plays it itself.
    void fallBackFromCapture(const std::string& reason);
    static constexpr F64 kCaptureDeliverySecs = 6.0;
    // Hands url to the plugin once SLMediaURLPolicy has cleared its host.
    void playChecked(const std::string& url);

    LLPluginClassMedia* initializeMedia(const std::string& media_type, bool allow_capture = true);

    LLPluginClassMedia *mMediaPlugin;

    void onStreamMetadata(const LLSD& tags);
    void clearMetadata();

    std::string mURL;
    std::string mPlayURL;   // what playChecked() last handed the plugin (the fallback replays it)
    // A host check answers asynchronously: it plays only if no start() / stop() came
    // since (generation) and this object still exists (token).
    U32 mStartGeneration = 0;
    std::shared_ptr<int> mAliveToken = std::make_shared<int>(0);
    F32 mGain;
    LLSD mMetadata;     // ARTIST / TITLE / TRSN (station) / URL, as the music ticker and stream info expect

    bool mCapture = false;          // this plugin instance was created with audio capture
    bool mCaptureFallback = false;  // #960: capture failed on this stream; the plugin plays it (until the next start())
    bool mCaptureDelivered = false; // the ring has received audio from this plugin instance
    F64  mCapturePlayingSince = -1.0;
    bool mCaptureProbe = true;
    F64  mProbeLastTime = -1.0;
    F64  mProbeNextLog = 0.0;
    F64  mProbeOwed = 0.0;          // frames the probe owes the ring (fractional)
    F32  mProbePeak = 0.f;
    bool mProbePrimed = false;
};


#endif //LL_VIEWERMEDIA_STREAMINGAUDIO_H
