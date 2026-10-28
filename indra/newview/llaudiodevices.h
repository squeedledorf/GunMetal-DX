/**
 * @file llaudiodevices.h
 * @brief #1122: one output and one input device choice for all of the viewer's audio.
 *
 * Preferences > Sound & Media > "Output device" / "Input device" (settings
 * SlipstreamAudioOutputDevice / SlipstreamAudioInputDevice: a device's display name, "" =
 * the system default -- the default, which is exactly the behaviour before this existed).
 *
 * The output choice applies to world audio (OpenAL, re-pointed with ALC_SOFT_reopen_device so
 * playing sounds carry on) and to voice output; the input choice to voice input (the viewer
 * records with nothing else). The voice-only pickers (VoiceInputAudioDevice /
 * VoiceOutputAudioDevice) stay: their "Default" entry now means "same as the main device", and a
 * specific voice device picked there still wins for voice. Precedence, per direction:
 *     VR headset override (LLVRAudio, temporary)  >  voice-only pick  >  this choice  >  system default
 * Media plugins (CEF / VLC / GStreamer, and parcel music through the media plugins) keep playing
 * on the system default (follow-up: on Linux a PULSE_SINK in the plugin's environment).
 *
 * Names are matched across the backends (OpenAL Soft and WebRTC name the same device slightly
 * differently): exact, then normalised (case, an "OpenAL Soft on " prefix, a trailing " [EDID]"
 * suffix), then one containing the other. A chosen device that is not connected is left in the
 * setting (never rewritten): audio then uses the system default until it is back.
 */
#pragma once

#include <string>
#include <vector>

class LLVoiceDevice;

class LLAudioDevices
{
public:
    // Once, after the audio engine started (llstartup): listens to the two settings and applies
    // the output choice to world audio (nothing at all with the default choice).
    static void init();

    // The choices ("" = the system default).
    static std::string outputChoice();
    static std::string inputChoice();

    // The entry of `names` that is `want` (see the matching above); -1 = none. strict = exact
    // and normalised only (no containment: for a name known exactly, e.g. PulseAudio's sink
    // description, where a sibling "... (HDMI 2)" must never stand in for "... (HDMI 4)").
    static int findDevice(const std::vector<std::string>& names, const std::string& want, bool strict = false);
    static bool sameDevice(const std::string& a, const std::string& b);

    // World audio: the OpenAL device name the output choice resolves to ("" = the system
    // default: no choice, or the chosen device is not connected).
    static std::string worldOutputDevice();
    // Puts world audio on the output choice, unless the VR headset holds it (LLVRAudio).
    // Enumerates only when a device is chosen or the engine is not on the default.
    static void applyWorldOutput();
    // Voice: the WebRTC device id (LLVoiceDevice::full_name) the choice resolves to in `devices`
    // ("" = none: the voice engine's "Default").
    static std::string voiceDeviceFor(bool capture, const std::vector<LLVoiceDevice>& devices);
    // A device list changed (WebRTC re-listed, i.e. plugged / unplugged): retries a chosen
    // output that was not connected.
    static void onDevicesChanged();
};
