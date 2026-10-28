/**
 * @file llvrdictation.h
 * @brief VR dictation (#1118, epic #1072): push-to-talk speech-to-text into the focused text
 *        field on the UI wall. Engine, capture and model: LLSpeechToText (llspeechtotext.h).
 *
 * The button: the RIGHT THUMBSTICK PRESS, held (Index / Touch; bound and unused until now --
 * the left stick press is the recentre hold, A / B / trigger are taken). A press that moves
 * the stick more than half way is not a dictation press.
 *  - The UI wall is up and a text field has keyboard focus (nearby chat, an IM, search, a
 *    notecard, ...): hold -> "Listening" pill above the field (and a haptic tick), speak,
 *    release -> "Transcribing" -> the text is typed into that field at its cursor, for
 *    editing. Never sent: the user presses Enter / Send as for typed text.
 *  - The wall is up but no text field has focus: a hint ("Select a text field first").
 *  - The wall is hidden: nothing (#1119 -- direct nearby chat -- plugs in here: see
 *    directChatEnabled() in llvrdictation.cpp and sendToNearbyChat()).
 * The speech model is not shipped: the first press asks to download it (size shown;
 * SHA-256 checked); the pill shows the progress.
 * While dictation listens, the voice-chat microphone is muted (audio_update_volume), so
 * dictated speech never reaches voice chat.
 *
 * Setting: SlipstreamVRDictationLanguage ("en" = English model; a language code or "auto"
 * = the multilingual model).
 *
 * Desktop / VR off: nothing here runs. tick() and cancel() are only called from
 * LLVRInput::update past its VR test, draw() and holdsVoiceMic() are one static read.
 */
#pragma once

#include "stdtypes.h"

#include <string>

struct LLXrInputState;

class LLVRDictation
{
public:
    // LLVRInput::update, every frame of a running session (after the controllers were read).
    // wall_up: the UI wall is shown this frame.
    static void tick(const LLXrInputState& in, bool wall_up);
    // LLVRInput::update while the session is not running (paused, dashboard, lost): stops a
    // capture without transcribing it. Cheap when idle.
    static void cancel();
    // The UI build, after the 2D UI (with the pointer's cursor): the listening / transcribing /
    // download pill and hints. Nothing when idle.
    static void draw();
    // Dictation is listening: voice chat's mic is muted meanwhile (llvieweraudio.cpp).
    static bool holdsVoiceMic();

    // #1119: say a transcript in nearby chat (channel 0), as typed chat (RLVa redirects /
    // sendchat filters apply in LLFloaterIMNearbyChat::sendChatFromViewer). Empty text is not
    // sent; a leading '/' is dropped (a transcript never becomes a channel / gesture command).
    // Returns false when nothing was sent.
    static bool sendToNearbyChat(const std::string& text);
};
