/**
* @file media_plugin_libvlc.cpp
* @brief LibVLC plugin for LLMedia API plugin system
*
* @cond
* $LicenseInfo:firstyear=2008&license=viewerlgpl$
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
* @endcond
*/

#include "linden_common.h"

#include "llpixelformat.h" // pixel format wire values for the texture_params message
#include "llplugininstance.h"
#include "llpluginmessage.h"
#include "llpluginmessageclasses.h"
#include "media_plugin_base.h"
#include "llpluginaudioring.h"

#if defined(_MSC_VER)
#include <basetsd.h>
typedef SSIZE_T ssize_t;
#endif

#include <chrono>
#include <cstring>
#include <mutex>
#include "vlc/vlc.h"
#include "vlc/libvlc_version.h"

#if LL_WINDOWS
// needed for waveOut call - see below for description
#include <mmsystem.h>
#endif

////////////////////////////////////////////////////////////////////////////////
//
class MediaPluginLibVLC :
    public MediaPluginBase
{
public:
    MediaPluginLibVLC(LLPluginInstance::sendMessageFunction host_send_func, void *host_user_data);
    ~MediaPluginLibVLC();

    /*virtual*/ void receiveMessage(const char* message_string);

private:
    bool init();

    void initVLC();
    void playMedia();
    void resetVLC();
    void setVolume(const F64 volume);
    void setVolumeVLC();
    void updateTitle(const char* title);
    void pollStreamMetadata();

    static void* lock(void* data, void** p_pixels);
    static void unlock(void* data, void* id, void* const* raw_pixels);
    static void display(void* data, void* id);

    /*virtual*/ void setDirty(int left, int top, int right, int bottom) /* override, but that is not supported in gcc 4.6 */;
    void setDurationDirty();

    static void eventCallbacks(const libvlc_event_t* event, void* ptr);

    // #892 audio capture: with the viewer's opt-in ("audio_capture"), every player playMedia()
    // builds gets libvlc_audio_set_format("S16N", 48000, 2) + libvlc_audio_set_callbacks before
    // play, so VLC hands its PCM to us (its own aout is then not used at all) and we copy it into
    // the viewer's shared ring. The callbacks run on VLC's audio thread: no plugin messages there.
    static void audioPlay(void* data, const void* samples, unsigned count, int64_t pts);
    static void audioPause(void* data, int64_t pts);
    static void audioResume(void* data, int64_t pts);
    static void audioFlush(void* data, int64_t pts);
    static void audioDrain(void* data);
    void applyAudioCapture();
    bool hasMemoryAudioOutput();
    void attachAudioSegment(const std::string& name);
    void sendAudioCaptureStatus(bool force);

    bool mAudioCaptureWanted = false;
    int mHasAmem = -1;                  // #960: libVLC has the "amem" audio output (-1: not checked yet)
    std::mutex mAudioMutex;             // guards mAudioWriter (VLC audio thread vs shm_remove)
    LLPluginAudioRingWriter mAudioWriter;
    std::string mAudioSegmentName;
    std::chrono::steady_clock::time_point mNextAudioStatus;

    libvlc_instance_t* mLibVLC;
    libvlc_media_t* mLibVLCMedia;
    libvlc_media_player_t* mLibVLCMediaPlayer;

    struct mLibVLCContext
    {
        unsigned char* texture_pixels;
        libvlc_media_player_t* mp;
        MediaPluginLibVLC* parent;
    };
    struct mLibVLCContext mLibVLCCallbackContext;

    std::string mURL;
    F64 mCurVolume;

    bool mIsLooping;

    F64 mCurTime;
    F64 mDuration;
    EStatus mVlcStatus;

    // #594: last stream tags sent to the viewer, and when to look again
    std::string mLastStreamMetadata;
    std::chrono::steady_clock::time_point mNextMetadataPoll;
};

////////////////////////////////////////////////////////////////////////////////
//
MediaPluginLibVLC::MediaPluginLibVLC(LLPluginInstance::sendMessageFunction host_send_func, void *host_user_data) :
MediaPluginBase(host_send_func, host_user_data)
{
    mTextureWidth = 0;
    mTextureHeight = 0;
    mWidth = 0;
    mHeight = 0;
    mDepth = 4;
    mPixels = 0;

    mLibVLC = 0;
    mLibVLCMedia = 0;
    mLibVLCMediaPlayer = 0;

    mCurVolume = 0.0;

    mIsLooping = false;

    mCurTime = 0.0;
    mDuration = 0.0;

    mURL = std::string();
    mNextMetadataPoll = std::chrono::steady_clock::now();

    mVlcStatus = STATUS_NONE;
    setStatus(STATUS_NONE);
}

////////////////////////////////////////////////////////////////////////////////
//
MediaPluginLibVLC::~MediaPluginLibVLC()
{
}

/////////////////////////////////////////////////////////////////////////////////
//
void* MediaPluginLibVLC::lock(void* data, void** p_pixels)
{
    struct mLibVLCContext* context = (mLibVLCContext*)data;

    *p_pixels = context->texture_pixels;

    return NULL;
}

/////////////////////////////////////////////////////////////////////////////////
//
void MediaPluginLibVLC::unlock(void* data, void* id, void* const* raw_pixels)
{
    // nothing to do here for the moment
    // we *could* modify pixels here to, for example, Y flip, but this is done with
    // a VLC video filter transform.
}

////////////////////////////////////////////////////////////////////////////////
//
void MediaPluginLibVLC::display(void* data, void* id)
{
    struct mLibVLCContext* context = (mLibVLCContext*)data;

    context->parent->setDirty(0, 0, context->parent->mWidth, context->parent->mHeight);
}

////////////////////////////////////////////////////////////////////////////////
//
void MediaPluginLibVLC::initVLC()
{
    char const* vlc_argv[] =
    {
        "--no-xlib",
        "--video-filter=transform{type=vflip}",  // MAINT-6578 Y flip textures in plugin vs client
    };

#if LL_DARWIN
    setenv("VLC_PLUGIN_PATH", "./plugins", 1);
#endif

    int vlc_argc = sizeof(vlc_argv) / sizeof(*vlc_argv);
    mLibVLC = libvlc_new(vlc_argc, vlc_argv);

    if (!mLibVLC)
    {
        // for the moment, if this fails, the plugin will fail and
        // the media sub-system will tell the viewer something went wrong.
    }
}

////////////////////////////////////////////////////////////////////////////////
//
void MediaPluginLibVLC::resetVLC()
{
    libvlc_media_player_stop(mLibVLCMediaPlayer);
    libvlc_media_player_release(mLibVLCMediaPlayer);
    libvlc_release(mLibVLC);
}

////////////////////////////////////////////////////////////////////////////////
// *virtual*
void MediaPluginLibVLC::setDirty(int left, int top, int right, int bottom)
{
    LLPluginMessage message(LLPLUGIN_MESSAGE_CLASS_MEDIA, "updated");

    message.setValueS32("left", left);
    message.setValueS32("top", top);
    message.setValueS32("right", right);
    message.setValueS32("bottom", bottom);

    message.setValueReal("current_time", mCurTime);
    message.setValueReal("duration", mDuration);
    message.setValueReal("current_rate", 1.0f);

    sendMessage(message);
}

////////////////////////////////////////////////////////////////////////////////
// *virtual*
void MediaPluginLibVLC::setDurationDirty()
{
    LLPluginMessage message(LLPLUGIN_MESSAGE_CLASS_MEDIA, "updated");

    message.setValueReal("current_time", mCurTime);
    message.setValueReal("duration", mDuration);
    message.setValueReal("current_rate", 1.0f);

    sendMessage(message);
}

////////////////////////////////////////////////////////////////////////////////
//
void MediaPluginLibVLC::eventCallbacks(const libvlc_event_t* event, void* ptr)
{
    MediaPluginLibVLC* parent = (MediaPluginLibVLC*)ptr;
    if (parent == 0)
    {
        return;
    }

    switch (event->type)
    {
    case libvlc_MediaPlayerOpening:
        parent->mVlcStatus = STATUS_LOADING;
        break;

    case libvlc_MediaPlayerPlaying:
        parent->mDuration = (float)(libvlc_media_get_duration(parent->mLibVLCMedia)) / 1000.0f;
        parent->mVlcStatus = STATUS_PLAYING;
        parent->setVolumeVLC();
        parent->setDurationDirty();
        break;

    case libvlc_MediaPlayerPaused:
        parent->mVlcStatus = STATUS_PAUSED;
        break;

    case libvlc_MediaPlayerStopped:
        parent->mVlcStatus = STATUS_DONE;
        break;

    case libvlc_MediaPlayerEndReached:
        parent->mVlcStatus = STATUS_DONE;
        parent->mCurTime = parent->mDuration;
        parent->setDurationDirty();
        break;

    case libvlc_MediaPlayerEncounteredError:
        parent->mVlcStatus = STATUS_ERROR;
        break;

    case libvlc_MediaPlayerTimeChanged:
        parent->mCurTime = (float)libvlc_media_player_get_time(parent->mLibVLCMediaPlayer) / 1000.0f;
        if (parent->mVlcStatus == STATUS_DONE && libvlc_media_player_is_playing(parent->mLibVLCMediaPlayer))
        {
            parent->mVlcStatus = STATUS_PLAYING;
        }
        parent->setDurationDirty();
        break;

    case libvlc_MediaPlayerPositionChanged:
        break;

    case libvlc_MediaPlayerLengthChanged:
        parent->mDuration = (float)libvlc_media_get_duration(parent->mLibVLCMedia) / 1000.0f;
        parent->setDurationDirty();
        break;

    case libvlc_MediaPlayerTitleChanged:
    {
        char* title = libvlc_media_get_meta(parent->mLibVLCMedia, libvlc_meta_Title);
        if (title)
        {
            parent->updateTitle(title);
        }
    }
    break;
    }
}

////////////////////////////////////////////////////////////////////////////////
// #892 audio capture callbacks (VLC audio output thread)
void MediaPluginLibVLC::audioPlay(void* data, const void* samples, unsigned count, int64_t pts)
{
    MediaPluginLibVLC* self = (MediaPluginLibVLC*)data;
    if (!self || !samples)
        return;
    // count = frames (samples per channel); S16N stereo as set by libvlc_audio_set_format.
    // pts: libvlc clock (us) at which these samples are due -- VLC 3 delivers ahead of it,
    // possibly in bursts; the ring absorbs that and the viewer paces playback itself.
    std::lock_guard<std::mutex> lock(self->mAudioMutex);
    self->mAudioWriter.write((const int16_t*)samples, (uint32_t)count, pts);
}

void MediaPluginLibVLC::audioPause(void* data, int64_t)
{
    MediaPluginLibVLC* self = (MediaPluginLibVLC*)data;
    std::lock_guard<std::mutex> lock(self->mAudioMutex);
    self->mAudioWriter.setState(LLPluginAudioRing::STATE_PAUSED);
}

void MediaPluginLibVLC::audioResume(void* data, int64_t)
{
    MediaPluginLibVLC* self = (MediaPluginLibVLC*)data;
    std::lock_guard<std::mutex> lock(self->mAudioMutex);
    self->mAudioWriter.setState(LLPluginAudioRing::STATE_PLAYING);
}

void MediaPluginLibVLC::audioFlush(void* data, int64_t)
{
    MediaPluginLibVLC* self = (MediaPluginLibVLC*)data;
    std::lock_guard<std::mutex> lock(self->mAudioMutex);
    self->mAudioWriter.flush();
}

void MediaPluginLibVLC::audioDrain(void* data)
{
    MediaPluginLibVLC* self = (MediaPluginLibVLC*)data;
    std::lock_guard<std::mutex> lock(self->mAudioMutex);
    self->mAudioWriter.setState(LLPluginAudioRing::STATE_STOPPED);
}

// #960: libvlc_audio_set_callbacks forces the player's audio output to "amem,none". Without the
// amem module (a packaging that leaves it out) VLC silently picks "none": the stream demuxes, but
// there is no audio output at all, so neither our callbacks nor the device ever get a sample.
bool MediaPluginLibVLC::hasMemoryAudioOutput()
{
    if (mHasAmem < 0 && mLibVLC)
    {
        mHasAmem = 0;
        libvlc_audio_output_t* outputs = libvlc_audio_output_list_get(mLibVLC);
        for (libvlc_audio_output_t* o = outputs; o; o = o->p_next)
        {
            if (o->psz_name && strcmp(o->psz_name, "amem") == 0)
            {
                mHasAmem = 1;
                break;
            }
        }
        if (outputs)
        {
            libvlc_audio_output_list_release(outputs);
        }
    }
    return mHasAmem > 0;
}

// Before libvlc_media_player_play, on every player we build (playMedia recreates it).
void MediaPluginLibVLC::applyAudioCapture()
{
    if (!mLibVLCMediaPlayer || !mAudioCaptureWanted)
        return;
    if (!hasMemoryAudioOutput())
    {
        // Capture cannot work: play the stream ourselves (normal output and volume) and tell the
        // viewer, which then stops expecting the ring to fill.
        mAudioCaptureWanted = false;
        {
            std::lock_guard<std::mutex> lock(mAudioMutex);
            mAudioWriter.detach();
            mAudioSegmentName.clear();
        }
        LLPluginMessage message(LLPLUGIN_MESSAGE_CLASS_MEDIA, "audio_capture_unavailable");
        message.setValue("reason", "libVLC has no \"amem\" audio output module");
        sendMessage(message);
        return;
    }
    libvlc_audio_set_format(mLibVLCMediaPlayer, "S16N", LLPluginAudioRing::kRate, LLPluginAudioRing::kChannels);
    libvlc_audio_set_callbacks(mLibVLCMediaPlayer, audioPlay, audioPause, audioResume, audioFlush, audioDrain, this);
    std::lock_guard<std::mutex> lock(mAudioMutex);
    mAudioWriter.flush();   // a new player is a new stream
}

void MediaPluginLibVLC::attachAudioSegment(const std::string& name)
{
    SharedSegmentMap::iterator iter = mSharedSegments.find(name);
    bool attached = false;
    {
        std::lock_guard<std::mutex> lock(mAudioMutex);
        mAudioWriter.detach();
        mAudioSegmentName.clear();
        if (iter != mSharedSegments.end() && mAudioWriter.attach(iter->second.mAddress, iter->second.mSize))
        {
            mAudioSegmentName = name;
            attached = true;
        }
    }
    if (!attached)
        return;
    LLPluginMessage message(LLPLUGIN_MESSAGE_CLASS_MEDIA, "audio_capture_format");
    message.setValueU32("rate", LLPluginAudioRing::kRate);
    message.setValueU32("channels", LLPluginAudioRing::kChannels);
    message.setValue("format", "S16");
    message.setValue("source", "libvlc audio callbacks");
    sendMessage(message);
    sendAudioCaptureStatus(true);
}

void MediaPluginLibVLC::sendAudioCaptureStatus(bool force)
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (!force && now < mNextAudioStatus)
        return;
    mNextAudioStatus = now + std::chrono::seconds(1);

    LLPluginMessage message(LLPLUGIN_MESSAGE_CLASS_MEDIA, "audio_capture_status");
    {
        std::lock_guard<std::mutex> lock(mAudioMutex);
        if (!mAudioWriter.attached())
            return;
        message.setValueReal("write_frames", (F64)mAudioWriter.writeFrames());
        message.setValueReal("pts_us", (F64)mAudioWriter.writePtsUs());
        message.setValueReal("dropped", (F64)mAudioWriter.droppedFrames());
        message.setValueU32("flushes", mAudioWriter.flushes());
        message.setValueU32("state", mAudioWriter.state());
        message.setValueBoolean("attached", mAudioCaptureWanted);
    }
    sendMessage(message);
}

////////////////////////////////////////////////////////////////////////////////
//
void MediaPluginLibVLC::playMedia()
{
    if (mURL.length() == 0)
    {
        return;
    }

    // A new call to play the media is received after the initial one. Typically
    // this is due to a size change request either as the media naturally resizes
    // to the size of the prim container, or else, as a 2D window is resized by the
    // user.  Stopping the media, helps avoid a race condition where the media pixel
    // buffer size is out of sync with the declared size (width/height) for a frame
    // or two and the plugin crashes as VLC tries to decode a frame into unallocated
    // memory.
    if (mLibVLCMediaPlayer)
    {
        libvlc_media_player_stop(mLibVLCMediaPlayer);
    }

    mLastStreamMetadata.clear();
    mLibVLCMedia = libvlc_media_new_location(mLibVLC, mURL.c_str());
    if (!mLibVLCMedia)
    {
        mLibVLCMediaPlayer = 0;
        setStatus(STATUS_ERROR);
        return;
    }

    mLibVLCMediaPlayer = libvlc_media_player_new_from_media(mLibVLCMedia);
    if (!mLibVLCMediaPlayer)
    {
        setStatus(STATUS_ERROR);
        return;
    }

    // listen to events
    libvlc_event_manager_t* em = libvlc_media_player_event_manager(mLibVLCMediaPlayer);
    if (em)
    {
        libvlc_event_attach(em, libvlc_MediaPlayerOpening, eventCallbacks, this);
        libvlc_event_attach(em, libvlc_MediaPlayerPlaying, eventCallbacks, this);
        libvlc_event_attach(em, libvlc_MediaPlayerPaused, eventCallbacks, this);
        libvlc_event_attach(em, libvlc_MediaPlayerStopped, eventCallbacks, this);
        libvlc_event_attach(em, libvlc_MediaPlayerEndReached, eventCallbacks, this);
        libvlc_event_attach(em, libvlc_MediaPlayerEncounteredError, eventCallbacks, this);
        libvlc_event_attach(em, libvlc_MediaPlayerTimeChanged, eventCallbacks, this);
        libvlc_event_attach(em, libvlc_MediaPlayerPositionChanged, eventCallbacks, this);
        libvlc_event_attach(em, libvlc_MediaPlayerLengthChanged, eventCallbacks, this);
        libvlc_event_attach(em, libvlc_MediaPlayerTitleChanged, eventCallbacks, this);
    }

    libvlc_video_set_callbacks(mLibVLCMediaPlayer, lock, unlock, display, &mLibVLCCallbackContext);
    applyAudioCapture();   // #892 (no-op unless the viewer asked for capture)
    libvlc_video_set_format(mLibVLCMediaPlayer, "RV32", mWidth, mHeight, mWidth * mDepth);

    mLibVLCCallbackContext.parent = this;
    mLibVLCCallbackContext.texture_pixels = mPixels;
    mLibVLCCallbackContext.mp = mLibVLCMediaPlayer;

    // Send a "navigate begin" event.
    // This is really a browser message but the QuickTime plugin did it and
    // the media system relies on this message to update internal state so we must send it too
    // Note: see "navigate_complete" message below too
    // https://jira.secondlife.com/browse/MAINT-6528
    LLPluginMessage message_begin(LLPLUGIN_MESSAGE_CLASS_MEDIA_BROWSER, "navigate_begin");
    message_begin.setValue("uri", mURL);
    message_begin.setValueBoolean("history_back_available", false);
    message_begin.setValueBoolean("history_forward_available", false);
    sendMessage(message_begin);

    // volume level gets set before VLC is initialized (thanks media system) so we have to record
    // it in mCurVolume and set it again here so that volume levels are correctly initialized
    setVolume(mCurVolume);

    setStatus(STATUS_LOADED);

    // note this relies on the "set_loop" message arriving before the "start" (play) one
    // but that appears to always be the case
    if (mIsLooping)
    {
        libvlc_media_add_option(mLibVLCMedia, "input-repeat=65535");
    }

    libvlc_media_player_play(mLibVLCMediaPlayer);

    // send a "location_changed" message - this informs the media system
    // that a new URL is the 'current' one and is used extensively.
    // Again, this is really a browser message but we will use it here.
    LLPluginMessage message(LLPLUGIN_MESSAGE_CLASS_MEDIA_BROWSER, "location_changed");
    message.setValue("uri", mURL);
    sendMessage(message);

    // Send a "navigate complete" event.
    // This is really a browser message but the QuickTime plugin did it and
    // the media system relies on this message to update internal state so we must send it too
    // Note: see "navigate_begin" message above too
    // https://jira.secondlife.com/browse/MAINT-6528
    LLPluginMessage message_complete(LLPLUGIN_MESSAGE_CLASS_MEDIA_BROWSER, "navigate_complete");
    message_complete.setValue("uri", mURL);
    message_complete.setValueS32("result_code", 200);
    message_complete.setValue("result_string", "OK");
    sendMessage(message_complete);
}

////////////////////////////////////////////////////////////////////////////////
//
void MediaPluginLibVLC::updateTitle(const char* title)
{
    LLPluginMessage message(LLPLUGIN_MESSAGE_CLASS_MEDIA, "name_text");
    message.setValue("name", title);
    sendMessage(message);
}

// #594: internet radio "now playing" tags (ICY StreamTitle, ID3 ...). VLC fills the media's
// meta as the stream delivers it, so look (throttled, from the plugin's own idle message so
// no VLC thread talks to the host) and tell the viewer only when something changed.
void MediaPluginLibVLC::pollStreamMetadata()
{
    if (!mLibVLCMedia)
    {
        return;
    }
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (now < mNextMetadataPoll)
    {
        return;
    }
    mNextMetadataPoll = now + std::chrono::milliseconds(1500);

    auto get_meta = [this](libvlc_meta_t type) -> std::string
    {
        std::string value;
        if (char* raw = libvlc_media_get_meta(mLibVLCMedia, type))
        {
            value = raw;
            libvlc_free(raw);
        }
        return value;
    };

    const std::string now_playing = get_meta(libvlc_meta_NowPlaying);
    const std::string title = get_meta(libvlc_meta_Title);
    const std::string artist = get_meta(libvlc_meta_Artist);
    const std::string genre = get_meta(libvlc_meta_Genre);
    const std::string url = get_meta(libvlc_meta_URL);

    if (now_playing.empty() && title.empty() && artist.empty())
    {
        return;
    }
    const std::string key = now_playing + "\x1f" + title + "\x1f" + artist + "\x1f" + genre + "\x1f" + url;
    if (key == mLastStreamMetadata)
    {
        return;
    }
    mLastStreamMetadata = key;

    LLPluginMessage message(LLPLUGIN_MESSAGE_CLASS_MEDIA, "stream_metadata");
    message.setValue("now_playing", now_playing);
    message.setValue("title", title);
    message.setValue("artist", artist);
    message.setValue("genre", genre);
    message.setValue("url", url);
    sendMessage(message);
}

void MediaPluginLibVLC::setVolumeVLC()
{
    if (mLibVLCMediaPlayer && mAudioCaptureWanted)
    {
        // #892: capturing. VLC applies volume in software ahead of our callbacks; keep it at
        // unity (the viewer applies the gain) and leave the process volume alone.
        libvlc_audio_set_volume(mLibVLCMediaPlayer, 100);
    }
    else if (mLibVLCMediaPlayer)
    {
        int vlc_vol = (int)(mCurVolume * 100);

        int result = libvlc_audio_set_volume(mLibVLCMediaPlayer, vlc_vol);
        if (result == 0)
        {
            // volume change was accepted by LibVLC
        }
        else
        {
            // volume change was NOT accepted by LibVLC and not actioned
        }

#if LL_WINDOWS
        // https ://jira.secondlife.com/browse/MAINT-8119
        // CEF media plugin uses code in media_plugins/cef/windows_volume_catcher.cpp to
        // set the actual output volume of the plugin process since there is no API in
        // CEF to otherwise do this.
        // There are explicit calls to change the volume in LibVLC but sometimes they
        // are ignored SLPlugin.exe process volume is set to 0 so you never heard audio
        // from the VLC media stream.
        // The right way to solve this is to move the volume catcher stuff out of
        // the CEF plugin and into it's own folder under media_plugins and have it referenced
        // by both CEF and VLC. That's for later. The code there boils down to this so for
                // now, as we approach a release, the less risky option is to do it directly vs
                // calls to volume catcher code.
        DWORD left_channel = (DWORD)(mCurVolume * 65535.0f);
        DWORD right_channel = (DWORD)(mCurVolume * 65535.0f);
        DWORD hw_volume = left_channel << 16 | right_channel;
        waveOutSetVolume(NULL, hw_volume);
#endif
    }
    else
    {
        // volume change was requested but VLC wasn't ready.
        // that's okay though because we saved the value in mCurVolume and
        // the next volume change after the VLC system is initilzied  will set it
    }
}

////////////////////////////////////////////////////////////////////////////////
//
void MediaPluginLibVLC::setVolume(const F64 volume)
{
    mCurVolume = volume;

    setVolumeVLC();
}

////////////////////////////////////////////////////////////////////////////////
//
void MediaPluginLibVLC::receiveMessage(const char* message_string)
{
    LLPluginMessage message_in;

    if (message_in.parse(message_string) >= 0)
    {
        std::string message_class = message_in.getClass();
        std::string message_name = message_in.getName();
        if (message_class == LLPLUGIN_MESSAGE_CLASS_BASE)
        {
            if (message_name == "init")
            {
                initVLC();

                LLPluginMessage message("base", "init_response");
                LLSD versions = LLSD::emptyMap();
                versions[LLPLUGIN_MESSAGE_CLASS_BASE] = LLPLUGIN_MESSAGE_CLASS_BASE_VERSION;
                versions[LLPLUGIN_MESSAGE_CLASS_MEDIA] = LLPLUGIN_MESSAGE_CLASS_MEDIA_VERSION;
                versions[LLPLUGIN_MESSAGE_CLASS_MEDIA_TIME] = LLPLUGIN_MESSAGE_CLASS_MEDIA_TIME_VERSION;
                message.setValueLLSD("versions", versions);

                std::ostringstream s;
                s << "LibVLC plugin ";
                s << LIBVLC_VERSION_MAJOR;
                s << ".";
                s << LIBVLC_VERSION_MINOR;
                s << ".";
                s << LIBVLC_VERSION_REVISION;

                message.setValue("plugin_version", s.str());
                sendMessage(message);
            }
            else if (message_name == "idle")
            {
                setStatus(mVlcStatus);
                pollStreamMetadata();
                if (!mAudioSegmentName.empty())
                {
                    sendAudioCaptureStatus(false);   // #892: ~1 Hz cursor / drop report
                }
            }
            else if (message_name == "cleanup")
            {
                resetVLC();
            }
            else if (message_name == "force_exit")
            {
                mDeleteMe = true;
            }
            else if (message_name == "shm_added")
            {
                SharedSegmentInfo info;
                info.mAddress = message_in.getValuePointer("address");
                info.mSize = (size_t)message_in.getValueS32("size");
                std::string name = message_in.getValue("name");

                mSharedSegments.insert(SharedSegmentMap::value_type(name, info));

            }
            else if (message_name == "shm_remove")
            {
                std::string name = message_in.getValue("name");

                SharedSegmentMap::iterator iter = mSharedSegments.find(name);
                if (iter != mSharedSegments.end())
                {
                    if (name == mAudioSegmentName)
                    {
                        // #892: VLC's audio thread must be done with the ring before we answer
                        // (the response lets the parent unmap it).
                        std::lock_guard<std::mutex> lock(mAudioMutex);
                        mAudioWriter.detach();
                        mAudioSegmentName.clear();
                    }
                    if (mPixels == iter->second.mAddress)
                    {
                        libvlc_media_player_stop(mLibVLCMediaPlayer);
                        libvlc_media_player_release(mLibVLCMediaPlayer);
                        mLibVLCMediaPlayer = 0;

                        mPixels = NULL;
                        mTextureSegmentName.clear();
                    }
                    mSharedSegments.erase(iter);
                }
                else
                {
                    //std::cerr << "MediaPluginWebKit::receiveMessage: unknown shared memory region!" << std::endl;
                }

                // Send the response so it can be cleaned up.
                LLPluginMessage message("base", "shm_remove_response");
                message.setValue("name", name);
                sendMessage(message);
            }
            else
            {
                //std::cerr << "MediaPluginWebKit::receiveMessage: unknown base message: " << message_name << std::endl;
            }
        }
        else if (message_class == LLPLUGIN_MESSAGE_CLASS_MEDIA)
        {
            if (message_name == "init")
            {
                LLPluginMessage message(LLPLUGIN_MESSAGE_CLASS_MEDIA, "texture_params");
                message.setValueS32("default_width", 1024);
                message.setValueS32("default_height", 1024);
                message.setValueS32("depth", mDepth);
                message.setValueU32("internalformat", static_cast<U32>(LLPixelFormat::RGB));
                message.setValueU32("format", static_cast<U32>(LLPixelFormat::BGRA));
                message.setValueU32("type", static_cast<U32>(LLPixelType::UBYTE));
                message.setValueBoolean("coords_opengl", true);
                sendMessage(message);
            }
            else if (message_name == "size_change")
            {
                std::string name = message_in.getValue("name");
                S32 width = message_in.getValueS32("width");
                S32 height = message_in.getValueS32("height");
                S32 texture_width = message_in.getValueS32("texture_width");
                S32 texture_height = message_in.getValueS32("texture_height");

                if (!name.empty())
                {
                    // Find the shared memory region with this name
                    SharedSegmentMap::iterator iter = mSharedSegments.find(name);
                    if (iter != mSharedSegments.end())
                    {
                        mPixels = (unsigned char*)iter->second.mAddress;
                        mWidth = width;
                        mHeight = height;
                        mTextureWidth = texture_width;
                        mTextureHeight = texture_height;

                        libvlc_time_t time = (libvlc_time_t)(1000.0 * mCurTime);

                        playMedia();

                        if (mLibVLCMediaPlayer)
                        {
                            libvlc_media_player_set_time(mLibVLCMediaPlayer, time);
                            time = libvlc_media_player_get_time(mLibVLCMediaPlayer);
                            if (time < 0)
                            {
                                // -1 if there is no media
                                mCurTime = 0;
                            }
                            else
                            {
                                mCurTime = (F64)time / 1000.0;
                            }
                        }
                    };
                };

                LLPluginMessage message(LLPLUGIN_MESSAGE_CLASS_MEDIA, "size_change_response");
                message.setValue("name", name);
                message.setValueS32("width", width);
                message.setValueS32("height", height);
                message.setValueS32("texture_width", texture_width);
                message.setValueS32("texture_height", texture_height);
                sendMessage(message);
            }
            else if (message_name == "load_uri")
            {
                mURL = message_in.getValue("uri");
                playMedia();
            }
            else if (message_name == "audio_capture")
            {
                // #892: opt-in from the viewer (parcel stream instance only). Normally arrives
                // before load_uri; the callbacks only take effect on the next player playMedia()
                // builds, so a later toggle applies from the next load / size change.
                mAudioCaptureWanted = message_in.getValueBoolean("enable");
                std::lock_guard<std::mutex> lock(mAudioMutex);
                mAudioWriter.setTestDiscard(message_in.hasValue("test_discard") && message_in.getValueBoolean("test_discard"));
                if (!mAudioCaptureWanted)
                {
                    mAudioWriter.detach();
                    mAudioSegmentName.clear();
                }
            }
            else if (message_name == "audio_capture_shm")
            {
                attachAudioSegment(message_in.getValue("name"));
            }
        }
        else
            if (message_class == LLPLUGIN_MESSAGE_CLASS_MEDIA_TIME)
            {
                if (message_name == "stop")
                {
                    if (mLibVLCMediaPlayer)
                    {
                        libvlc_media_player_stop(mLibVLCMediaPlayer);
                    }
                }
                else if (message_name == "start")
                {
                    if (mLibVLCMediaPlayer)
                    {
                        if (mVlcStatus == STATUS_DONE && !libvlc_media_player_is_playing(mLibVLCMediaPlayer))
                        {
                            // stop or vlc will ignore 'play', it will just
                            // make an MediaPlayerEndReached event even if
                            // seek was used
                            libvlc_media_player_stop(mLibVLCMediaPlayer);
                        }
                        libvlc_media_player_play(mLibVLCMediaPlayer);
                    }
                }
                else if (message_name == "pause")
                {
                    if (mLibVLCMediaPlayer)
                    {
                        libvlc_media_player_set_pause(mLibVLCMediaPlayer, 1);
                    }
                }
                else if (message_name == "seek")
                {
                    if (mLibVLCMediaPlayer)
                    {
                        libvlc_time_t time = (libvlc_time_t)(1000.0 * message_in.getValueReal("time"));
                        libvlc_media_player_set_time(mLibVLCMediaPlayer, time);
                        time = libvlc_media_player_get_time(mLibVLCMediaPlayer);
                        if (time < 0)
                        {
                            // -1 if there is no media
                            mCurTime = 0;
                        }
                        else
                        {
                            mCurTime = (F64)time / 1000.0;
                        }

                        if (!libvlc_media_player_is_playing(mLibVLCMediaPlayer))
                        {
                            // if paused, won't trigger update, update now
                            setDurationDirty();
                        }
                    }
                }
                else if (message_name == "set_loop")
                {
                    bool loop = message_in.getValueBoolean("loop");
                    mIsLooping = loop;
                }
                else if (message_name == "set_volume")
                {
                    // volume comes in 0 -> 1.0
                    F64 volume = message_in.getValueReal("volume");
                    setVolume(volume);
                }
            }
    }
}

////////////////////////////////////////////////////////////////////////////////
//
bool MediaPluginLibVLC::init()
{
    LLPluginMessage message(LLPLUGIN_MESSAGE_CLASS_MEDIA, "name_text");
    message.setValue("name", "LibVLC Plugin");
    sendMessage(message);

    return true;
};

////////////////////////////////////////////////////////////////////////////////
//
int init_media_plugin(LLPluginInstance::sendMessageFunction host_send_func,
    void* host_user_data,
    LLPluginInstance::sendMessageFunction *plugin_send_func,
    void **plugin_user_data)
{
    MediaPluginLibVLC* self = new MediaPluginLibVLC(host_send_func, host_user_data);
    *plugin_send_func = MediaPluginLibVLC::staticReceiveMessage;
    *plugin_user_data = (void*)self;

    return 0;
}
