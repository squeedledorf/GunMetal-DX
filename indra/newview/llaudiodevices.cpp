/**
 * @file llaudiodevices.cpp
 * @brief #1122: one output and one input device choice for all of the viewer's audio (llaudiodevices.h),
 *        and its preferences panel (panel_audio_devices.xml).
 */

#include "llviewerprecompiledheaders.h"

#include "llaudiodevices.h"

#include "llaudioengine.h"
#include "llcombobox.h"
#include "llpanel.h"
#include "lluictrlfactory.h"
#include "llviewercontrol.h"
#include "llvoiceclient.h"
#include "llvoicewebrtc.h"
#include "llvraudio.h"

#include <algorithm>
#include <cctype>

namespace
{
const char* const kOutputSetting = "SlipstreamAudioOutputDevice";
const char* const kInputSetting = "SlipstreamAudioInputDevice";

// PulseAudio / PipeWire list each output's monitor as a capture source: never an input choice.
bool isMonitorSource(const std::string& name)
{
    return name.rfind("Monitor of ", 0) == 0;
}

std::string lowered(const std::string& s)
{
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](char c) { return (char)std::tolower(static_cast<unsigned char>(c)); });
    return out;
}

// The comparable form of a device name: lower case, without OpenAL Soft's old "OpenAL Soft on "
// prefix and without the " [<EDID monitor name>]" suffix OpenAL Soft appends to HDMI / DP
// outputs on PipeWire (WebRTC lists the same sink without it).
std::string normalised(const std::string& name)
{
    std::string n = lowered(name);
    static const std::string prefix("openal soft on ");
    if (n.rfind(prefix, 0) == 0) n.erase(0, prefix.size());
    const size_t br = n.rfind(" [");
    if (br != std::string::npos && !n.empty() && n.back() == ']') n.erase(br);
    while (!n.empty() && std::isspace(static_cast<unsigned char>(n.back()))) n.pop_back();
    while (!n.empty() && std::isspace(static_cast<unsigned char>(n.front()))) n.erase(0, 1);
    return n;
}

std::string s_loggedWorld;   // the last world-output decision logged (no repeats)
} // namespace

// static
std::string LLAudioDevices::outputChoice()
{
    return gSavedSettings.getString(kOutputSetting);
}

// static
std::string LLAudioDevices::inputChoice()
{
    return gSavedSettings.getString(kInputSetting);
}

// static
bool LLAudioDevices::sameDevice(const std::string& a, const std::string& b)
{
    return a == b || (!a.empty() && normalised(a) == normalised(b));
}

// static
int LLAudioDevices::findDevice(const std::vector<std::string>& names, const std::string& want, bool strict)
{
    if (want.empty()) return -1;
    for (size_t i = 0; i < names.size(); ++i)
        if (names[i] == want) return (int)i;
    const std::string w = normalised(want);
    if (w.empty()) return -1;
    for (size_t i = 0; i < names.size(); ++i)
        if (normalised(names[i]) == w) return (int)i;
    if (strict) return -1;
    // One containing the other (e.g. PipeWire's " Mono" / " Stereo" on one side only): the
    // closest in length wins.
    int best = -1;
    size_t best_diff = std::string::npos;
    for (size_t i = 0; i < names.size(); ++i)
    {
        const std::string n = normalised(names[i]);
        if (n.empty()) continue;
        if (n.find(w) == std::string::npos && w.find(n) == std::string::npos) continue;
        const size_t diff = n.size() > w.size() ? n.size() - w.size() : w.size() - n.size();
        if (diff < best_diff)
        {
            best_diff = diff;
            best = (int)i;
        }
    }
    return best;
}

// static
std::string LLAudioDevices::worldOutputDevice()
{
    const std::string choice = outputChoice();
    if (choice.empty() || !gAudiop) return std::string();
    const std::vector<std::string> names = gAudiop->listOutputDevices();
    const int idx = findDevice(names, choice);
    return idx >= 0 ? names[idx] : std::string();
}

// static
void LLAudioDevices::applyWorldOutput()
{
    if (!gAudiop) return;
    const std::string choice = outputChoice();
    // The default choice with the engine on the default: today's behaviour, nothing enumerated.
    if (choice.empty() && gAudiop->currentOutputDevice().empty()) return;
    if (LLVRAudio::holdsWorldOutput())
    {
        // The VR session's headset output sits on top; it goes back to this choice when it ends.
        return;
    }
    const std::string target = worldOutputDevice();
    std::string decision;
    if (choice.empty())
        decision = "world audio: output device 'System default'";
    else if (target.empty())
        decision = "world audio: output device '" + choice + "' is not connected; using the system default until it is";
    else
        decision = "world audio: output device '" + choice + "' -> OpenAL '" + target + "'";
    if (decision != s_loggedWorld)
    {
        s_loggedWorld = decision;
        LL_INFOS("AudioDevices") << decision << LL_ENDL;
    }
    if (target == gAudiop->currentOutputDevice()) return;
    if (!gAudiop->switchOutputDevice(target))
    {
        LL_WARNS("AudioDevices") << "world audio: could not move to '" << (target.empty() ? "<system default>" : target)
                                 << "'; it stays on '"
                                 << (gAudiop->currentOutputDevice().empty() ? "<system default>" : gAudiop->currentOutputDevice())
                                 << "'" << LL_ENDL;
    }
}

// static
std::string LLAudioDevices::voiceDeviceFor(bool capture, const std::vector<LLVoiceDevice>& devices)
{
    const std::string choice = capture ? inputChoice() : outputChoice();
    if (choice.empty() || devices.empty()) return std::string();
    std::vector<std::string> names;
    names.reserve(devices.size());
    for (const LLVoiceDevice& d : devices)
        names.push_back((capture && isMonitorSource(d.display_name)) ? std::string() : d.display_name);
    const int idx = findDevice(names, choice);
    return idx >= 0 ? devices[idx].full_name : std::string();
}

// static
void LLAudioDevices::onDevicesChanged()
{
    if (!outputChoice().empty()) applyWorldOutput();
}

namespace
{
void applyVoice()
{
    if (LLWebRTCVoiceClient::instanceExists()) LLWebRTCVoiceClient::instance().applyDevices();
}
} // namespace

// static
void LLAudioDevices::init()
{
    static bool s_done = false;
    if (!s_done)
    {
        s_done = true;
        if (LLControlVariable* c = gSavedSettings.getControl(kOutputSetting))
        {
            c->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) {
                LL_INFOS("AudioDevices") << "output device choice: '" << (outputChoice().empty() ? "System default" : outputChoice())
                                         << "'" << LL_ENDL;
                applyWorldOutput();
                applyVoice();
            });
        }
        if (LLControlVariable* c = gSavedSettings.getControl(kInputSetting))
        {
            c->getSignal()->connect([](LLControlVariable*, const LLSD&, const LLSD&) {
                LL_INFOS("AudioDevices") << "input device choice: '" << (inputChoice().empty() ? "System default" : inputChoice())
                                         << "'" << LL_ENDL;
                applyVoice();
            });
        }
    }
    if (!outputChoice().empty() || !inputChoice().empty())
    {
        LL_INFOS("AudioDevices") << "device choices: output '" << (outputChoice().empty() ? "System default" : outputChoice())
                                 << "', input '" << (inputChoice().empty() ? "System default" : inputChoice()) << "'" << LL_ENDL;
    }
    applyWorldOutput();
}

// ---- Preferences > Sound & Media: the two device pickers ----

class LLPanelAudioDevices : public LLPanel
{
public:
    bool postBuild() override
    {
        mOutput = getChild<LLComboBox>("audio_output_device");
        mInput = getChild<LLComboBox>("audio_input_device");
        // Re-listed when opened (devices come and go); the list only changes if the names did.
        mOutput->setMouseDownCallback([this](LLUICtrl*, S32, S32, MASK) { fill(false); });
        mInput->setMouseDownCallback([this](LLUICtrl*, S32, S32, MASK) { fill(true); });
        return true;
    }

    void onVisibilityChange(bool new_visibility) override
    {
        LLPanel::onVisibilityChange(new_visibility);
        if (new_visibility)
        {
            // (Only when the voice engine has no lists yet: a re-list re-applies its devices,
            // which nobody who never opens the pickers should get. Opening a list re-lists.)
            if (LLWebRTCVoiceClient::instanceExists())
            {
                LLWebRTCVoiceClient& voice = LLWebRTCVoiceClient::instance();
                if (voice.getRenderDevices().empty() || voice.getCaptureDevices().empty()) voice.refreshDeviceLists(false);
            }
            fill(false);
            fill(true);
        }
    }

private:
    void fill(bool capture)
    {
        LLComboBox* combo = capture ? mInput : mOutput;
        if (!combo) return;
        std::vector<std::string> names;
        auto add = [&names, capture](const std::string& n) {
            if (n.empty() || (capture && isMonitorSource(n))) return;
            for (const std::string& e : names)
                if (LLAudioDevices::sameDevice(e, n)) return;
            names.push_back(n);
        };
        // OpenAL's names first (world audio's), then the voice engine's.
        if (gAudiop)
            for (const std::string& n : capture ? gAudiop->listInputDevices() : gAudiop->listOutputDevices()) add(n);
        if (LLWebRTCVoiceClient::instanceExists())
        {
            LLWebRTCVoiceClient& voice = LLWebRTCVoiceClient::instance();
            for (const LLVoiceDevice& d : capture ? voice.getCaptureDevices() : voice.getRenderDevices()) add(d.display_name);
        }
        const std::string choice = capture ? LLAudioDevices::inputChoice() : LLAudioDevices::outputChoice();
        // A chosen device that is not connected stays listed (and chosen): unplugging it is not
        // the user changing their mind.
        const bool missing = !choice.empty() && LLAudioDevices::findDevice(names, choice) < 0;
        std::vector<std::string>& listed = capture ? mInputListed : mOutputListed;
        std::vector<std::string> wanted = names;
        if (missing) wanted.push_back(choice);
        if (wanted == listed && combo->getItemCount() > 0) return;
        listed = wanted;
        combo->removeall();
        combo->add(getString("system_default"), LLSD(std::string()), ADD_BOTTOM);
        for (const std::string& n : names) combo->add(n, LLSD(n), ADD_BOTTOM);
        if (missing) combo->add(choice + " " + getString("not_connected"), LLSD(choice), ADD_BOTTOM);
        combo->setValue(LLSD(choice));
    }

    LLComboBox* mOutput = nullptr;
    LLComboBox* mInput = nullptr;
    std::vector<std::string> mOutputListed, mInputListed;
};

static LLPanelInjector<LLPanelAudioDevices> t_panel_audio_devices("panel_audio_devices");
