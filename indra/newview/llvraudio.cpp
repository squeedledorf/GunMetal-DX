/**
 * @file llvraudio.cpp
 * @brief VR (#1122): the viewer's audio follows the headset while a VR session runs (llvraudio.h).
 */

#include "llviewerprecompiledheaders.h"

#include "llvraudio.h"

#include "llaudiodevices.h"
#include "llaudioengine.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llvoicewebrtc.h"

#include "llvraudiopulse.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <vector>

namespace
{
// ---- The matcher table ----
// Case-insensitive substrings of a device's name, in priority order (the first rule any device
// matches wins; among devices matching that rule, the first listed). Names as the audio
// backends report them:
//  - Linux (PipeWire / PulseAudio, OpenAL Soft and WebRTC see the same descriptions): an HDMI /
//    DisplayPort sink is "<GPU codec> Digital Stereo (HDMI n) [<EDID monitor name>]", e.g.
//    "GB202 High Definition Audio Controller Digital Stereo (HDMI) [M28U]" -- the Valve Index's
//    DisplayPort audio carries its EDID name ("Index HMD"). Its microphone is the USB device
//    "Valve VR Radio & HMD Mic" (" Mono" appended by PipeWire).
//  - Windows (endpoint friendly names): "<endpoint> (<device>)", e.g. "Headphones (Oculus
//    Virtual Audio Device)", "Microphone (Valve VR Radio & HMD Mic)", "Index HMD (NVIDIA High
//    Definition Audio)".
struct Rule
{
    const char* token;
    const char* label;
};

const Rule kOutputRules[] = {
    {"Index HMD", "Valve Index (DisplayPort audio, EDID name)"},
    {"Valve Index", "Valve Index"},
    {"Oculus Virtual Audio", "Meta Quest Link / Air Link"},
    {"Rift S", "Oculus Rift S"},
    {"Virtual Desktop Audio", "Virtual Desktop"},
    {"Steam Streaming Speakers", "Steam Link (streamed headset)"},
    {"ALVR", "ALVR"},
    {"Reverb G2", "HP Reverb G2"},
    {"Mixed Reality", "Windows Mixed Reality headset"},
    {"VIVE Pro", "HTC VIVE Pro"},
    {"VIVE Cosmos", "HTC VIVE Cosmos"},
    {"HTC Vive", "HTC Vive"},
    {"Bigscreen Beyond", "Bigscreen Beyond"},
    {"Valve", "Valve (generic)"},
};

const Rule kMicRules[] = {
    {"Valve VR Radio & HMD Mic", "Valve Index microphone (USB)"},
    {"Index HMD", "Valve Index"},
    {"Oculus Virtual Audio", "Meta Quest Link / Air Link"},
    {"Rift S", "Oculus Rift S"},
    {"Virtual Desktop Audio", "Virtual Desktop"},
    {"Steam Streaming Microphone", "Steam Link (streamed headset)"},
    {"ALVR", "ALVR"},
    {"Reverb G2", "HP Reverb G2"},
    {"Mixed Reality", "Windows Mixed Reality headset"},
    {"VIVE Pro", "HTC VIVE Pro"},
    {"VIVE Cosmos", "HTC VIVE Cosmos"},
    {"HTC Vive", "HTC Vive"},
    {"Bigscreen Beyond", "Bigscreen Beyond"},
};

bool containsNoCase(const std::string& hay, const char* needle)
{
    const std::string n(needle);
    auto it = std::search(hay.begin(), hay.end(), n.begin(), n.end(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
    return it != hay.end();
}

// PulseAudio / PipeWire list each output's monitor as a capture source: never a microphone.
bool isMonitorSource(const std::string& name)
{
    return name.rfind("Monitor of ", 0) == 0;
}

template <size_t N> const char* matchRules(const Rule (&rules)[N], const std::string& name)
{
    if (name.empty()) return nullptr;
    for (const Rule& r : rules)
    {
        if (containsNoCase(name, r.token)) return r.label;
    }
    return nullptr;
}

// The best match in a list (rule priority first, then list order): index, -1 = none.
template <size_t N, typename NameOf>
int bestMatch(const Rule (&rules)[N], size_t count, NameOf name_of, const char** label_out)
{
    for (const Rule& r : rules)
    {
        for (size_t i = 0; i < count; ++i)
        {
            const std::string name = name_of(i);
            if (!name.empty() && containsNoCase(name, r.token))
            {
                if (label_out) *label_out = r.label;
                return (int)i;
            }
        }
    }
    return -1;
}

bool isDefaultVoiceDevice(const std::string& id)
{
    return id.empty() || id == "Default";
}

// WebRTC names an HDMI / DP output without the EDID monitor name OpenAL Soft appends ("... (HDMI 4)"
// vs "... (HDMI 4) [Index HMD]"): a voice output equal to an OpenAL name minus its " [...]" suffix
// gets that OpenAL name added, so the headset rules see the EDID name.
std::string withEdidName(const std::string& voice_name, const std::vector<std::string>& openal_names)
{
    for (const std::string& n : openal_names)
    {
        const size_t br = n.rfind(" [");
        if (br != std::string::npos && n.back() == ']' && n.compare(0, br, voice_name) == 0 && br == voice_name.size())
            return voice_name + " | " + n;
    }
    return voice_name;
}

constexpr F64 kOutputRetrySec = 5.0;    // headset output not found yet: look again this often
constexpr F64 kOutputRetryForSec = 60.0;    // ... for this long after the session started

// ---- State (main thread only) ----
bool s_running = false;
F64  s_sessionStart = 0.0;
// World audio (OpenAL).
bool s_outputDecided = false;   // switched, already the default, unavailable, or gave up
bool s_outputSwitched = false;  // we moved OpenAL: restore it on the session's end
F64  s_outputNextTry = 0.0;
bool s_engineSeen = false;      // the audio engine existed for a try this session
bool s_outputOnHeadset = false; // world audio plays on the headset (the gain applies)
bool s_outputLimiter = false;   // ... and was reopened with the output limiter (gain > 0 dB)
F32  s_appliedBoost = 1.f;      // the output boost set on the engine
// Linux: the headset's card port found through PulseAudio / PipeWire (llvraudiopulse.h); its
// sink's description is then the headset output's exact name for OpenAL and voice.
LLVRAudioPulse::Headset s_pulse;
// Voice output: WebRTC re-lists devices only on its own events (a card profile switch is not
// one), so while no headset output was found the list is refreshed on the output's schedule.
bool s_voiceOutFound = false;
F64  s_voiceNextRefresh = 0.0;
// Voice.
bool s_userCapture = false;     // the user picked a voice device during this session
bool s_userRender = false;
std::string s_micName, s_micId;
std::string s_voiceLogged;      // the last voice decision logged (no repeats on list refreshes)
bool s_voiceOnHeadset = false;  // voice output plays on the headset (the gain applies)

// The headset gain (SlipstreamVRAudioGainDb), clamped: -20..+12 dB.
F32 headsetGainDb()
{
    static LLCachedControl<F32> s_db(gSavedSettings, "SlipstreamVRAudioGainDb", 6.f);
    return llclamp((F32)s_db, -20.f, 12.f);
}

F32 headsetGainLinear()
{
    return std::pow(10.f, headsetGainDb() / 20.f);
}

bool wantLimiter()
{
    return headsetGainDb() > 0.05f;
}

// World audio's output boost: the headset gain while world audio is on the headset, else 1.
void applyOutputBoost()
{
    const F32 boost = (s_running && s_outputOnHeadset) ? headsetGainLinear() : 1.f;
    if (!gAudiop || std::fabs(boost - s_appliedBoost) < 0.0005f) return;
    s_appliedBoost = boost;
    gAudiop->setOutputBoost(boost);
    LL_INFOS("VRAudio") << "world audio: headset gain " << (boost == 1.f ? 0.f : headsetGainDb()) << " dB (x" << boost << ")"
                        << LL_ENDL;
}

LLVRAudio::changed_signal_t& changedSignal()
{
    static LLVRAudio::changed_signal_t s_signal;
    return s_signal;
}

void logOutputList(const std::vector<std::string>& names)
{
    LL_INFOS("VRAudio") << "world audio (OpenAL) output devices: " << names.size() << LL_ENDL;
    for (const std::string& n : names)
    {
        const char* rule = LLVRAudio::matchHeadsetOutput(n);
        LL_INFOS("VRAudio") << "  '" << n << "'" << (rule ? std::string(" <- headset: ") + rule : std::string()) << LL_ENDL;
    }
}

// World audio: decided once per session (retried while the headset output is not listed yet;
// the device list is logged on the first and the last try).
void resolveOutput(bool first_try, bool last_try)
{
    if (!gAudiop)
    {
        // The session can start on the login screen, before the audio engine exists: wait for it
        // (LLVRAudio::update starts the retry window when it appears).
        if (first_try)
            LL_INFOS("VRAudio") << "world audio: no audio engine yet; moving it when it starts" << LL_ENDL;
        return;
    }
    // Linux: PulseAudio found the headset's port but its sink was not listed yet: look again.
    if (!first_try && !s_pulse.card.empty() && s_pulse.sink_desc.empty()) LLVRAudioPulse::refreshSink(s_pulse);

    const std::vector<std::string> names = gAudiop->listOutputDevices();
    const std::string def = gAudiop->defaultOutputDevice();
    if (first_try || last_try)
    {
        logOutputList(names);
        LL_INFOS("VRAudio") << "world audio: system default output '" << def << "', engine on '"
                            << (gAudiop->currentOutputDevice().empty() ? "<system default>" : gAudiop->currentOutputDevice())
                            << "'" << LL_ENDL;
    }

    std::string target;
    const char* label = nullptr;
    // 1. The sink PulseAudio found by the card port's EDID name (exactly its description).
    if (!s_pulse.sink_desc.empty())
    {
        const int idx = LLAudioDevices::findDevice(names, s_pulse.sink_desc, true);
        if (idx >= 0)
        {
            target = names[idx];
            label = "its card port, via PulseAudio";
        }
        else if (!last_try)
        {
            // (OpenAL lists a new sink a moment after it appears: wait for it rather than settle
            // for a name rule.)
            if (first_try)
                LL_INFOS("VRAudio") << "world audio: waiting for OpenAL to list the headset sink '" << s_pulse.sink_desc << "'"
                                    << LL_ENDL;
            return;
        }
    }
    else if (!s_pulse.card.empty() && !last_try)
    {
        return;   // (the port was found; its sink is not there yet: refreshSink above retries)
    }
    // 2. The name rules (Windows endpoint names; Linux sinks that carry the EDID name).
    if (target.empty())
    {
        const int idx = bestMatch(kOutputRules, names.size(), [&names](size_t i) { return names[i]; }, &label);
        if (idx >= 0) target = names[idx];
    }
    // 3. The system default is the headset (SteamVR's "switch audio device") but not listed as such.
    if (target.empty() && gAudiop->currentOutputDevice().empty())
    {
        label = LLVRAudio::matchHeadsetOutput(def);
        if (label) target = def;
    }
    if (target.empty())
    {
        if (last_try)
        {
            LL_INFOS("VRAudio") << "world audio: no headset output among " << names.size()
                                << " devices; world audio stays on the desktop output. (Linux: the headset's "
                                   "DisplayPort sink must be enabled in the sound card's profile.)"
                                << LL_ENDL;
            s_outputDecided = true;
        }
        else if (first_try)
        {
            LL_INFOS("VRAudio") << "world audio: no headset output among " << names.size() << " devices yet; looking again every "
                                << kOutputRetrySec << " s for " << kOutputRetryForSec << " s" << LL_ENDL;
        }
        return;
    }
    // Moved there by name even when it already plays there (the system default, or the user's
    // chosen output): the headset gain needs the output limiter, and the session's end then
    // puts world audio back on the user's choice.
    const bool limiter = wantLimiter();
    LL_INFOS("VRAudio") << "world audio: headset output '" << target << "' (" << label << "); moving world audio there"
                        << (limiter ? " with the output limiter" : "") << LL_ENDL;
    s_outputSwitched = gAudiop->switchOutputDevice(target, limiter);
    s_outputOnHeadset = s_outputSwitched;
    s_outputLimiter = s_outputSwitched && limiter;
    if (!s_outputSwitched)
    {
        LL_WARNS("VRAudio") << "world audio: could not move to the headset; it stays on the desktop output" << LL_ENDL;
    }
    s_outputDecided = true;
    applyOutputBoost();
}

void restoreOutput()
{
    s_outputOnHeadset = false;
    s_outputLimiter = false;
    applyOutputBoost();   // (back to 1)
    if (!s_outputSwitched) return;
    s_outputSwitched = false;
    if (!gAudiop) return;
    // Back to the user's output choice (Sound preferences; "" = the system default, as opened).
    const std::string user = LLAudioDevices::worldOutputDevice();
    LL_INFOS("VRAudio") << "world audio: back to " << (user.empty() ? std::string("the system default output") : "'" + user + "' (the chosen output device)")
                        << LL_ENDL;
    if (!gAudiop->switchOutputDevice(user))
    {
        LL_WARNS("VRAudio") << "world audio: could not go back; it stays on '" << gAudiop->currentOutputDevice() << "'" << LL_ENDL;
    }
}

// Voice: the override for one direction. "" = none needed / possible. Logs into `why`.
std::string voiceOverrideFor(bool capture, const LLVoiceDeviceList& devices, const std::string& saved,
                             const std::string& system_default, std::string& why, std::string* found_name,
                             std::string* found_id)
{
    const char* what = capture ? "input" : "output";
    if (capture ? s_userCapture : s_userRender)
    {
        why += std::string(" ") + what + ": the user's pick (no override);";
        // Still look the mic up for the dictation hook.
    }
    const char* label = nullptr;
    if (!capture) s_voiceOnHeadset = false;
    // Output: the sink PulseAudio found by the headset's card port first (Linux).
    int pulse_idx = -1;
    if (!capture && !s_pulse.sink_desc.empty())
    {
        std::vector<std::string> shown;
        for (const LLVoiceDevice& d : devices) shown.push_back(d.display_name);
        pulse_idx = LLAudioDevices::findDevice(shown, s_pulse.sink_desc, true);
        if (pulse_idx >= 0) label = "its card port, via PulseAudio";
    }
    const std::vector<std::string> openal = (!capture && gAudiop && pulse_idx < 0) ? gAudiop->listOutputDevices() : std::vector<std::string>();
    const int idx = pulse_idx >= 0 ? pulse_idx : capture ? bestMatch(kMicRules, devices.size(),
                                        [&devices](size_t i) {
                                            const LLVoiceDevice& d = devices[i];
                                            return isMonitorSource(d.display_name) ? std::string() : d.display_name + " | " + d.full_name;
                                        },
                                        &label)
                            : bestMatch(kOutputRules, devices.size(),
                                        [&devices, &openal](size_t i) {
                                            return withEdidName(devices[i].display_name, openal) + " | " + devices[i].full_name;
                                        },
                                        &label);
    if (idx < 0)
    {
        why += std::string(" ") + what + ": no headset device among " + std::to_string(devices.size()) + ";";
        return std::string();
    }
    if (!capture) s_voiceOutFound = true;
    const LLVoiceDevice& dev = devices[idx];
    if (found_name) *found_name = dev.display_name;
    if (found_id) *found_id = dev.full_name;
    if (capture ? s_userCapture : s_userRender) return std::string();
    // (Every path below leaves voice output on the headset: already there, or the override.)
    if (!capture) s_voiceOnHeadset = true;
    if (saved == dev.full_name)
    {
        why += std::string(" ") + what + ": the saved device already is the headset's '" + dev.display_name + "';";
        return std::string();
    }
    if (isDefaultVoiceDevice(saved))
    {
        const char* def_rule = capture ? LLVRAudio::matchHeadsetMic(system_default) : LLVRAudio::matchHeadsetOutput(system_default);
        if (def_rule)
        {
            why += std::string(" ") + what + ": the system default already is the headset ('" + system_default + "');";
            return std::string();
        }
    }
    why += std::string(" ") + what + ": headset '" + dev.display_name + "' (" + label + ");";
    return dev.full_name;
}

void resolveVoice()
{
    if (!LLWebRTCVoiceClient::instanceExists()) return;
    LLWebRTCVoiceClient& voice = LLWebRTCVoiceClient::instance();
    const std::string def_out = gAudiop ? gAudiop->defaultOutputDevice() : std::string();
    const std::string def_in = gAudiop ? gAudiop->defaultInputDevice() : std::string();
    std::string why;
    std::string mic_name, mic_id;
    // ("saved" = the user's device without the override: the voice-only pick, else the main
    // device choice of Sound preferences, else "Default".)
    const std::string capture = voiceOverrideFor(true, voice.getCaptureDevices(), voice.userCaptureDevice(), def_in, why,
                                                 &mic_name, &mic_id);
    const std::string render = voiceOverrideFor(false, voice.getRenderDevices(), voice.userRenderDevice(), def_out, why,
                                                nullptr, nullptr);
    if (why != s_voiceLogged)
    {
        s_voiceLogged = why;
        LL_INFOS("VRAudio") << "voice (" << voice.getCaptureDevices().size() << " capture, " << voice.getRenderDevices().size()
                            << " render devices; system default input '" << def_in << "', output '" << def_out << "'):" << why
                            << LL_ENDL;
    }
    voice.setDeviceOverride(capture, render);
    if (mic_name != s_micName || mic_id != s_micId)
    {
        s_micName = mic_name;
        s_micId = mic_id;
        LL_INFOS("VRAudio") << "headset microphone (for dictation): "
                            << (s_micName.empty() ? std::string("none") : "'" + s_micName + "'") << LL_ENDL;
        changedSignal()();
    }
}

void sessionStarted()
{
    LL_INFOS("VRAudio") << "VR session running: moving the viewer's audio to the headset" << LL_ENDL;
    s_sessionStart = LLTimer::getTotalSeconds();
    s_outputDecided = s_outputSwitched = false;
    s_userCapture = s_userRender = false;
    s_voiceLogged.clear();
    s_engineSeen = gAudiop != nullptr;
    s_voiceOutFound = false;
    s_voiceOnHeadset = false;
    s_outputOnHeadset = s_outputLimiter = false;
    s_voiceNextRefresh = s_sessionStart + kOutputRetrySec;
    // Linux: the headset's output port by its EDID name among the sound cards' ports (switches
    // the card's profile to it when needed; bounded, logged). Elsewhere / not found: the name rules.
    LLVRAudioPulse::findHeadsetOutput(&LLVRAudio::matchHeadsetOutput, s_pulse);
    if (!s_pulse.profile_set.empty() && LLWebRTCVoiceClient::instanceExists())
    {
        // (A profile switch is no device event for WebRTC: have it list the new sink now.)
        LLWebRTCVoiceClient::instance().refreshDeviceLists(false);
    }
    resolveOutput(true, false);
    s_outputNextTry = s_sessionStart + kOutputRetrySec;
    resolveVoice();
    changedSignal()();
}

void sessionEnded()
{
    LL_INFOS("VRAudio") << "VR session stopped: the viewer's audio goes back to the desktop devices" << LL_ENDL;
    restoreOutput();
    if (LLWebRTCVoiceClient::instanceExists())
    {
        LLWebRTCVoiceClient::instance().setDeviceOverride(std::string(), std::string());
    }
    // The sound card back to the profile it had (after world audio and voice left its sink).
    LLVRAudioPulse::restore(s_pulse);
    s_pulse = LLVRAudioPulse::Headset();
    s_outputDecided = false;
    s_engineSeen = false;
    s_voiceOutFound = false;
    s_voiceOnHeadset = false;
    s_userCapture = s_userRender = false;
    s_micName.clear();
    s_micId.clear();
    s_voiceLogged.clear();
    changedSignal()();
}
} // namespace

// static
void LLVRAudio::update(bool running)
{
    if (running == s_running)
    {
        if (running && s_outputOnHeadset && gAudiop)
        {
            // The headset gain is live: the limiter follows its sign (one reopen when it
            // crosses 0 dB), the boost its value.
            const bool limiter = wantLimiter();
            if (limiter != s_outputLimiter)
            {
                s_outputLimiter = limiter;
                gAudiop->switchOutputDevice(gAudiop->currentOutputDevice(), limiter);
            }
            applyOutputBoost();
        }
        if (running && !s_voiceOutFound && !s_userRender && LLWebRTCVoiceClient::instanceExists())
        {
            const F64 now = LLTimer::getTotalSeconds();
            if (now >= s_voiceNextRefresh && now - s_sessionStart < kOutputRetryForSec)
            {
                s_voiceNextRefresh = now + kOutputRetrySec;
                LLWebRTCVoiceClient::instance().refreshDeviceLists(false);   // (-> onVoiceDevicesChanged)
            }
        }
        if (running && !s_outputDecided)
        {
            const F64 now = LLTimer::getTotalSeconds();
            if (!gAudiop) return;   // (no engine yet: the window starts when it does)
            if (!s_engineSeen)
            {
                // The engine came up during the session: a fresh first try and retry window.
                s_engineSeen = true;
                s_sessionStart = now;
                s_outputNextTry = now + kOutputRetrySec;
                resolveOutput(true, false);
            }
            else if (now >= s_outputNextTry)
            {
                s_outputNextTry = now + kOutputRetrySec;
                resolveOutput(false, now - s_sessionStart >= kOutputRetryForSec);
            }
        }
        return;
    }
    s_running = running;
    if (running)
        sessionStarted();
    else
        sessionEnded();
}

// static
void LLVRAudio::onVoiceDevicesChanged()
{
    if (!s_running) return;
    resolveVoice();
}

// static
void LLVRAudio::userPickedVoiceDevice(bool capture)
{
    if (!s_running) return;
    bool& flag = capture ? s_userCapture : s_userRender;
    if (flag) return;
    flag = true;
    if (!capture) s_voiceOnHeadset = false;
    LL_INFOS("VRAudio") << "voice: the user picked a voice " << (capture ? "input" : "output")
                        << " device; the headset override for it ends for this session" << LL_ENDL;
    if (LLWebRTCVoiceClient::instanceExists())
    {
        LLWebRTCVoiceClient& voice = LLWebRTCVoiceClient::instance();
        voice.setDeviceOverride(capture ? std::string() : voice.captureOverride(),
                                capture ? voice.renderOverride() : std::string());
    }
}

// static
bool LLVRAudio::active()
{
    return s_running;
}

// static
bool LLVRAudio::holdsWorldOutput()
{
    return s_outputSwitched;
}

// static
float LLVRAudio::voiceOutputBoost()
{
    return (s_running && s_voiceOnHeadset) ? headsetGainLinear() : 1.f;
}

// static
void LLVRAudio::shutdown()
{
    if (!s_running) return;
    LL_INFOS("VRAudio") << "viewer quitting during a VR session: putting the audio devices back" << LL_ENDL;
    s_running = false;
    sessionEnded();
}

// static
const std::string& LLVRAudio::headsetMicName()
{
    return s_micName;
}

// static
const std::string& LLVRAudio::headsetMicVoiceId()
{
    return s_micId;
}

// static
boost::signals2::connection LLVRAudio::addChangedCallback(const changed_signal_t::slot_type& cb)
{
    return changedSignal().connect(cb);
}

// static
const char* LLVRAudio::matchHeadsetOutput(const std::string& name)
{
    return matchRules(kOutputRules, name);
}

// static
const char* LLVRAudio::matchHeadsetMic(const std::string& name)
{
    if (isMonitorSource(name)) return nullptr;
    return matchRules(kMicRules, name);
}
