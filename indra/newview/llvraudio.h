/**
 * @file llvraudio.h
 * @brief VR (#1122, epic #1072): the viewer's audio follows the headset while a VR session runs.
 *
 * On the session's start (READY -> xrBeginSession) the headset's output and microphone are
 * looked up by name -- OpenXR has no portable way to name them -- with a small table of known
 * headsets (kOutputRules / kMicRules in llvraudio.cpp; every candidate, rule and decision is
 * logged under the tag "VRAudio"):
 *  - Linux first asks PulseAudio / PipeWire (llvraudiopulse.h): the headset's EDID name is
 *    only on its sound card PORT (the Index's DisplayPort audio is one port of the GPU's card,
 *    enabled by a card profile, and its sink is named after the codec). The card is switched to
 *    a profile with that port when needed, and the sink's description is then the exact device
 *    name for OpenAL and voice. Not available / nothing found: the name rules below;
 *  - world audio (OpenAL) moves to the headset output with ALC_SOFT_reopen_device: the open
 *    device is re-pointed, so playing sounds carry on. Not found at the start (the headset's
 *    DisplayPort sink can appear a little later): looked for again every 5 s for 60 s;
 *  - voice (WebRTC) output and input get a temporary override (LLWebRTCVoiceClient::
 *    setDeviceOverride), re-resolved on every voice device-list change during the session;
 *  - the headset gain (SlipstreamVRAudioGainDb, default +6 dB) on world audio (the engine's
 *    output boost, with OpenAL's output limiter above 0 dB) and voice (its receive volume)
 *    while they play on the headset.
 * Already on the headset (SteamVR's "switch audio device" made it the system default, or the
 * user's device is the headset's): world audio is re-pointed to it by name anyway (the gain
 * needs the limiter); voice is left alone.
 * On the session's end (or loss, or quit: shutdown()) everything goes back: OpenAL to the
 * user's output choice (LLAudioDevices; the system default unless one was chosen), the voice
 * overrides cleared, the gain removed, the sound card's profile restored (if it still has the
 * one we set). Settings are never written. A device the user picks in the voice device
 * preferences during the session ends that override for the session.
 *
 * Media plugins (CEF / VLC / GStreamer) keep playing on the system default (follow-up).
 *
 * Desktop (no VR): update(false) is one bool compare per frame; nothing is enumerated and
 * libpulse is never loaded.
 */
#pragma once

#include <boost/signals2.hpp>
#include <string>

class LLVRAudio
{
public:
    // Once per frame (LLAppViewer::doFrame, main thread): running = a VR session is running.
    // Acts on the edges only (plus the bounded output retry while running).
    static void update(bool running);
    // LLWebRTCVoiceClient::OnDevicesChangedImpl (main thread), after the lists were refilled:
    // while a session runs, re-resolves the headset voice devices (sets the override).
    static void onVoiceDevicesChanged();
    // The user picked a voice device in the preferences (capture = input): during a session that
    // override ends until the session does.
    static void userPickedVoiceDevice(bool capture);

    // The headset audio devices are in use (a session runs).
    static bool active();
    // World audio was moved to the headset (LLAudioDevices leaves it alone until it is restored).
    static bool holdsWorldOutput();
    // The headset gain (SlipstreamVRAudioGainDb) as a linear factor while voice output plays on
    // the headset during a session, else 1 (audio_update_volume multiplies the voice volume).
    static float voiceOutputBoost();
    // LLAppViewer::cleanup, before the audio engine goes: a session still running at quit is
    // ended (world audio / voice / the sound card's profile put back).
    static void shutdown();

    // ---- Hooks for speech-to-text / dictation (#1118, #1119) ----
    // The headset microphone found while a session runs ("" = none / no session): its display
    // name and its voice-engine (WebRTC) device id (LLVoiceDevice::full_name). Set whether or
    // not voice needed an override (it may already be the default).
    static const std::string& headsetMicName();
    static const std::string& headsetMicVoiceId();
    // Fired (main thread) when active() changes or the headset mic found changes.
    typedef boost::signals2::signal<void()> changed_signal_t;
    static boost::signals2::connection addChangedCallback(const changed_signal_t::slot_type& cb);

    // The matcher table: the rule (a short label) a device name matches, nullptr = none.
    static const char* matchHeadsetOutput(const std::string& name);
    static const char* matchHeadsetMic(const std::string& name);
};
