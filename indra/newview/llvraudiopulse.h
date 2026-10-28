/**
 * @file llvraudiopulse.h
 * @brief VR (#1122): the headset's output on Linux through PulseAudio / PipeWire (libpulse,
 *        loaded at run time with dlopen; no link-time dependency).
 *
 * A headset whose speakers are the GPU's HDMI / DisplayPort audio (Valve Index) is one port of
 * the GPU's sound card, and such a card exposes ONE HDMI / DP output at a time through its card
 * profiles: the headset's sink only exists once the card is switched to a profile with its port.
 * Its sink is then named after the codec only ("... Digital Stereo (HDMI 4)"): the headset's
 * EDID name is only on the card PORT, as its "device.product.name" property. So the headset is
 * found among the cards' ports (the same name rules as llvraudio.cpp), the card is switched to
 * a profile with that port when needed (remembering the profile it had), and the resulting
 * sink's description -- the name OpenAL Soft and WebRTC show -- is what world audio and voice
 * use. On the session's end the card goes back to its profile (only if it still has the one we
 * set).
 *
 * Every call is synchronous on the calling (main) thread with a hard deadline (~1.5 s; a local
 * server answers in milliseconds), and connects with PA_CONTEXT_NOAUTOSPAWN. Only called while
 * a VR session starts / runs / ends: libpulse is not even loaded otherwise. Linux only; the
 * other platforms (and a Linux build without the PulseAudio headers) get stubs returning false.
 */
#pragma once

#include <string>

namespace LLVRAudioPulse
{
struct Headset
{
    std::string card;            // card name (e.g. alsa_card.pci-0000_81_00.1)
    std::string card_desc;       // its description (for the log)
    std::string port;            // the headset's port (e.g. hdmi-output-3)
    std::string matched;         // the name that matched a rule (EDID product name or port description)
    std::string rule;            // the rule's label
    std::string profile_before;  // the card's profile before ("" = not switched)
    std::string profile_set;     // the profile we switched it to ("" = it already had the port)
    std::string sink;            // the headset's sink name ("" = not there yet)
    std::string sink_desc;       // its description: the device name OpenAL / WebRTC show
};

typedef const char* (*match_fn_t)(const std::string& name);

// Finds the headset's output port (match = the output name rules) and makes its sink exist
// (switches the card's profile when the port is not in the active one). false = libpulse
// missing, no server, or no port matches (all logged under VRAudio); `out` then has no card.
bool findHeadsetOutput(match_fn_t match, Headset& out);
// Looks the headset's sink up again (it can appear a moment after the profile switch): fills
// sink / sink_desc; false = not there (yet).
bool refreshSink(Headset& h);
// Puts the card back to profile_before if we switched it and it still has profile_set.
void restore(const Headset& h);
} // namespace LLVRAudioPulse
