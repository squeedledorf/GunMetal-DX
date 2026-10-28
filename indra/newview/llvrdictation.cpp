/**
 * @file llvrdictation.cpp
 * @brief VR dictation (#1118). See llvrdictation.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrdictation.h"

#include "llagent.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llfloaterimnearbychat.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llframetimer.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llspeechtotext.h"
#include "lluictrl.h"
#include "llviewercontrol.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvraudio.h"
#include "llworld.h"
#include "rlvactions.h"
#include "vkgfx/llxrsession.h"

#include <algorithm>
#include <cmath>

namespace
{
enum class State : U8
{
    IDLE,
    LISTENING,      // the button is held, the mic captures
    TRANSCRIBING,   // released: the clip is with the worker
};

enum class Target : U8
{
    FIELD,          // the text field that had keyboard focus at the press
    NEARBY_CHAT,    // #1119: said in nearby chat
};

constexpr F32 kMaxClipSec = 29.5f;      // whisper's window is 30 s: released for the user there
constexpr F32 kStickPushed = 0.5f;      // a press that moves the stick this far is not dictation
constexpr F32 kTapSec = 0.35f;          // a press released sooner is a tap: send the field (Enter)
constexpr F64 kHintSec = 3.0;
constexpr F32 kNearbyRadius = 20.f;     // normal chat range (m): whose names may be spoken
constexpr size_t kMaxUserTerms = 48;
constexpr size_t kMaxNearbyPeople = 16;
constexpr size_t kMaxTermChars = 48;    // longer is not a name (parcel names are often slogans)

struct DictationState
{
    State state = State::IDLE;
    Target target = Target::FIELD;
    // The field at the press. Only compared with the current keyboard focus, and only used
    // while it still is the focus (it may be gone by the time the text is ready).
    LLFocusableElement* field = nullptr;
    U32 serial = 0;                     // the clip being transcribed
    F64 releasedAt = 0.0;
    F32 clipSec = 0.f;
    bool prevButton = false;
    bool consentOpen = false;           // the download question is up
    const LLSpeechToText::Model* model = nullptr;   // the model for the clip being recorded
    bool upgradeOffered = false;       // the bigger model was offered this session (once)
    bool watching = false;              // connected to LLVRAudio's changed signal
    std::string hint;
    F64 hintUntil = 0.0;
};
DictationState sD;

// #1119 (push-to-talk straight to nearby chat with the UI hidden) turns this on, with its
// own cancel path; until then a press with the wall hidden does nothing.
bool directChatEnabled()
{
    return false;
}

F64 now()
{
    return LLFrameTimer::getElapsedSeconds();
}

std::string dictationLanguage()
{
    static LLCachedControl<std::string> s_lang(gSavedSettings, "SlipstreamVRDictationLanguage", "en");
    return s_lang;
}

void addTerm(std::vector<std::string>& out, std::string term)
{
    LLStringUtil::trim(term);
    if (term.empty() || term.size() > kMaxTermChars) return;
    for (unsigned char c : term)
        if (c < 0x20 || c == 0x7f) return;
    out.push_back(std::move(term));
}

// Words the speaker is likely to say, most important first, for the speech model's prompt
// (LLSpeechToText::transcribe): the user's own list (SlipstreamDictationVocabulary,
// comma-separated), the display / legacy names of avatars within chat range (closest first),
// then the region and parcel names. RLVa: no names or places the restrictions hide. Built per
// dictation on the main thread; never logged.
std::vector<std::string> dictationVocabulary()
{
    std::vector<std::string> v;

    static LLCachedControl<std::string> s_vocab(gSavedSettings, "SlipstreamDictationVocabulary", "");
    const std::string list = s_vocab;
    size_t from = 0, user_terms = 0;
    while (from <= list.size() && user_terms < kMaxUserTerms)
    {
        size_t to = list.find_first_of(",;\n", from);
        if (to == std::string::npos) to = list.size();
        const size_t before = v.size();
        addTerm(v, list.substr(from, to - from));
        user_terms += v.size() - before;
        from = to + 1;
    }

    if (!RlvActions::isRlvEnabled() || RlvActions::canShowNearbyAgents())
    {
        uuid_vec_t ids;
        std::vector<LLVector3d> positions;
        const LLVector3d here = gAgent.getPositionGlobal();
        LLWorld::getInstance()->getAvatars(&ids, &positions, here, kNearbyRadius);
        std::vector<std::pair<F64, LLUUID>> by_distance;
        for (size_t i = 0; i < ids.size() && i < positions.size(); ++i)
        {
            if (ids[i] != gAgentID) by_distance.emplace_back(dist_vec_squared(positions[i], here), ids[i]);
        }
        std::sort(by_distance.begin(), by_distance.end(),
                  [](const std::pair<F64, LLUUID>& a, const std::pair<F64, LLUUID>& b) { return a.first < b.first; });
        if (by_distance.size() > kMaxNearbyPeople) by_distance.resize(kMaxNearbyPeople);
        for (const auto& p : by_distance)
        {
            LLAvatarName name;
            if (!LLAvatarNameCache::get(p.second, &name)) continue;
            if (RlvActions::isRlvEnabled() && !RlvActions::canShowName(RlvActions::SNC_DEFAULT, p.second)) continue;
            addTerm(v, name.getDisplayName());
            std::string legacy = name.getLegacyName();
            const std::string resident = " Resident";
            if (legacy.size() > resident.size() && legacy.compare(legacy.size() - resident.size(), resident.size(), resident) == 0)
                legacy.resize(legacy.size() - resident.size());
            addTerm(v, legacy);
        }
    }

    if (!RlvActions::isRlvEnabled() || RlvActions::canShowLocation())
    {
        if (LLViewerRegion* region = gAgent.getRegion()) addTerm(v, region->getName());
        LLViewerParcelMgr* parcels = LLViewerParcelMgr::getInstance();
        if (parcels->getAgentParcel()) addTerm(v, parcels->getAgentParcelName());
    }
    return v;
}

void haptic(F32 amplitude, F32 seconds)
{
    if (LLXrSession::active()) LLXrSession::instance().haptic(LLXR_HAND_RIGHT, amplitude, seconds);
}

void showHint(const std::string& text)
{
    sD.hint = text;
    sD.hintUntil = now() + kHintSec;
}

// Everything back to idle; the model is freed and the worker stopped (the VR session ended).
void releaseAll()
{
    sD.state = State::IDLE;
    sD.field = nullptr;
    sD.serial = 0;
    sD.hint.clear();
    LLSpeechToText::release();
}

// The session's end frees the model (free on liveness; nothing stays loaded with VR off).
// Connected on the first dictation press, never before.
void watchSession()
{
    if (sD.watching) return;
    sD.watching = true;
    LLVRAudio::addChangedCallback([]()
    {
        if (!LLVRAudio::active()) releaseAll();
    });
}

LLUICtrl* textTarget(LLFocusableElement* focus)
{
    LLUICtrl* ctrl = dynamic_cast<LLUICtrl*>(focus);
    if (!ctrl || !ctrl->acceptsTextInput() || !ctrl->getEnabled() || !ctrl->isInVisibleChain()) return nullptr;
    return ctrl;
}

// Types the text into the field at its cursor, as keyboard text input arrives
// (LLViewerWindow::handleUnicodeChar -> the keyboard focus), so every text control takes it
// the same way (line / text editors, the chat bar, combo box entries; undo works).
void typeInto(LLUICtrl* ctrl, const std::string& text)
{
    std::string utf8 = text;
    const std::string have = ctrl->getValue().asString();
    if (!have.empty() && have.back() != ' ' && have.back() != '\n') utf8 = " " + utf8;
    const LLWString w = utf8str_to_wstring(utf8);
    for (llwchar c : w)
    {
        if (c < 0x20 || c == 0x7f) continue;   // (no Enter: nothing is ever sent)
        ctrl->handleUnicodeChar(c, false);
    }
}

void askToDownload(const LLSpeechToText::Model& m)
{
    if (sD.consentOpen) return;
    sD.consentOpen = true;
    const LLSpeechToText::Model* model = &m;   // (static table)
    LLSD args;
    args["SIZE"] = llformat("%.0f", (F64)m.bytes / 1.0e6);
    // (a model with a fallback is the bigger, more accurate one)
    args["KIND"] = m.multilingual ? "multilingual" : LLSpeechToText::fallbackFor(m) ? "English, more accurate" : "English";
    LLNotificationsUtil::add("VRDictationDownloadModel", args, LLSD(),
        [model](const LLSD& notification, const LLSD& response)
        {
            sD.consentOpen = false;
            if (LLNotificationsUtil::getSelectedOption(notification, response) != 0) return;
            LL_INFOS("SpeechToText") << "The user agreed to download the speech model " << model->file << LL_ENDL;
            LLSpeechToText::downloadModel(*model, [](bool ok)
            {
                LLNotificationsUtil::add(ok ? "VRDictationModelReady" : "VRDictationModelFailed");
            });
        });
}

void onPress(bool wall_up)
{
    if (!LLSpeechToText::available())
    {
        showHint("Dictation is not available here (it needs a CPU with AVX2)");
        return;
    }
    Target target = Target::FIELD;
    LLFocusableElement* field = nullptr;
    if (wall_up)
    {
        field = gFocusMgr.getKeyboardFocus();
        if (!textTarget(field))
        {
            showHint("Select a text field first, then hold the right stick to dictate");
            haptic(0.15f, 0.02f);
            return;
        }
    }
    else
    {
        if (!directChatEnabled()) return;
        target = Target::NEARBY_CHAT;
    }
    if (sD.state == State::TRANSCRIBING)
    {
        showHint("Still transcribing...");
        return;
    }
    const std::string language = dictationLanguage();
    const LLSpeechToText::Model* use = &LLSpeechToText::modelFor(language);
    if (LLSpeechToText::downloading()) return;   // (the pill shows the progress)
    if (!LLSpeechToText::modelPresent(*use))
    {
        // The preferred (bigger) model is not here yet: dictate with the smaller one that is, and
        // offer the download once per session. With none on disk, the download is asked for first.
        const LLSpeechToText::Model* fallback = LLSpeechToText::fallbackFor(*use);
        if (!fallback || !LLSpeechToText::modelPresent(*fallback))
        {
            askToDownload(*use);
            return;
        }
        if (!sD.upgradeOffered)
        {
            sD.upgradeOffered = true;
            askToDownload(*use);
        }
        use = fallback;
    }
    sD.model = use;
    watchSession();
    if (!LLSpeechToText::startCapture(LLVRAudio::headsetMicName()))
    {
        showHint("No microphone found for dictation");
        return;
    }
    sD.state = State::LISTENING;
    sD.target = target;
    sD.field = field;
    sD.hint.clear();
    haptic(0.35f, 0.03f);
}

void onRelease()
{
    std::vector<S16> pcm = LLSpeechToText::stopCapture();
    haptic(0.2f, 0.02f);
    sD.clipSec = (F32)pcm.size() / 16000.f;
    // A short TAP (not dictation) sends the focused field: Enter through the keyboard path, so the
    // field commits exactly as the Enter key would (nearby chat says it; RLVa applies there).
    if (sD.target == Target::FIELD && sD.clipSec < kTapSec)
    {
        sD.state = State::IDLE;
        LLFocusableElement* focus = gFocusMgr.getKeyboardFocus();
        LLUICtrl* ctrl = (focus && focus == sD.field) ? textTarget(focus) : nullptr;
        sD.field = nullptr;
        if (ctrl && !ctrl->getValue().asString().empty() && gViewerWindow)
        {
            gViewerWindow->handleKey(KEY_RETURN, MASK_NONE);
            LL_INFOS("SpeechToText") << "dictation: tap -> Enter on the focused field" << LL_ENDL;
        }
        else
        {
            showHint("Hold the right stick to dictate, tap it to send");
        }
        return;
    }
    if (!LLSpeechToText::trimSilence(pcm))
    {
        LL_INFOS("SpeechToText") << "dictation: " << sD.clipSec << " s clip held no speech; nothing transcribed" << LL_ENDL;
        sD.state = State::IDLE;
        showHint("Nothing heard");
        return;
    }
    if (!sD.model) sD.model = &LLSpeechToText::modelFor(dictationLanguage());
    sD.serial = LLSpeechToText::transcribe(std::move(pcm), dictationLanguage(), *sD.model, dictationVocabulary());
    if (!sD.serial)
    {
        sD.state = State::IDLE;
        return;
    }
    sD.state = State::TRANSCRIBING;
    sD.releasedAt = now();
}

void deliver(const LLSpeechToText::Result& r)
{
    sD.state = State::IDLE;
    const F64 latency = now() - sD.releasedAt;
    // (The transcript itself is the user's speech: never logged.)
    LL_INFOS("SpeechToText") << "dictation: " << sD.clipSec << " s held, " << r.audioSec << " s of speech, inference "
                             << (S32)r.inferMs << " ms, release -> text " << (S32)(latency * 1000.0) << " ms, "
                             << r.text.size() << " chars" << (r.ok ? "" : ", failed: " + r.error) << LL_ENDL;
    if (!r.ok)
    {
        showHint("Dictation failed: " + r.error);
        return;
    }
    if (r.text.empty())
    {
        showHint("Nothing recognised");
        return;
    }
    if (sD.target == Target::NEARBY_CHAT)
    {
        LLVRDictation::sendToNearbyChat(r.text);
        return;
    }
    LLFocusableElement* focus = gFocusMgr.getKeyboardFocus();
    LLUICtrl* ctrl = (focus && focus == sD.field) ? textTarget(focus) : nullptr;
    sD.field = nullptr;
    if (!ctrl)
    {
        showHint("The text field lost focus; nothing was typed");
        return;
    }
    // A new dictation REPLACES what the field holds (owner: so a bad result can just be tried
    // again); the field's own value setter clears it, then the text arrives as typed input.
    ctrl->setValue(LLSD(std::string()));
    typeInto(ctrl, r.text);
}

// The pill: a dark rounded-off box with a status dot and one line of text, in scaled UI
// coordinates (the caller set the UI scale).
void drawPill(S32 x, S32 y, const std::string& text, const LLColor4& dot)
{
    const LLFontGL* font = LLFontGL::getFontSansSerif();
    if (!font) return;
    const S32 pad = 6, dot_w = 16;
    const S32 w = font->getWidth(text) + 2 * pad + dot_w;
    const S32 h = font->getLineHeight() + 2 * pad;
    const S32 win_w = gViewerWindow->getWindowWidthScaled(), win_h = gViewerWindow->getWindowHeightScaled();
    x = llclamp(x, 4, llmax(4, win_w - w - 4));
    y = llclamp(y, 4, llmax(4, win_h - h - 4));
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gl_rect_2d(x, y + h, x + w, y, LLColor4(0.f, 0.f, 0.f, 0.78f), true);
    gGL.color4fv(dot.mV);
    gl_circle_2d((F32)(x + pad + 5), (F32)(y + h / 2), 5.f, 16, true);
    gGL.flush();
    font->renderUTF8(text, 0, (F32)(x + pad + dot_w), (F32)(y + pad), LLColor4::white, LLFontGL::LEFT, LLFontGL::BOTTOM,
                     LLFontGL::NORMAL, LLFontGL::NO_SHADOW);
}
} // namespace

void LLVRDictation::tick(const LLXrInputState& in, bool wall_up)
{
    // Results first (a release last frame may have its text by now).
    if (sD.state == State::TRANSCRIBING || LLSpeechToText::busy())
    {
        LLSpeechToText::Result r;
        while (LLSpeechToText::pollResult(r))
        {
            if (sD.state == State::TRANSCRIBING && r.serial == sD.serial) deliver(r);
        }
    }

    const LLXrHandState& rh = in.hand[LLXR_HAND_RIGHT];
    const bool button = in.focused && rh.active && rh.stickClick;
    const F32 pushed = std::sqrt(rh.stick[0] * rh.stick[0] + rh.stick[1] * rh.stick[1]);
    if (button && !sD.prevButton && pushed <= kStickPushed) onPress(wall_up);
    sD.prevButton = button;

    if (sD.state != State::LISTENING) return;
    if (!in.focused || !rh.active)
    {
        cancel();   // dashboard / controller gone: nothing is transcribed
        return;
    }
    LLSpeechToText::pollCapture();
    if (!button || LLSpeechToText::captureSeconds() >= kMaxClipSec) onRelease();
}

void LLVRDictation::cancel()
{
    if (sD.state != State::LISTENING) return;
    LLSpeechToText::stopCapture();   // (discarded)
    sD.state = State::IDLE;
    sD.field = nullptr;
    LL_INFOS("SpeechToText") << "dictation cancelled (session lost focus)" << LL_ENDL;
}

bool LLVRDictation::holdsVoiceMic()
{
    return sD.state == State::LISTENING;
}

void LLVRDictation::draw()
{
    const bool downloading = LLSpeechToText::downloading();
    if (sD.state == State::IDLE && !downloading && sD.hintUntil == 0.0) return;
    if (!LLXrSession::active() || !gViewerWindow) return;
    std::string text;
    LLColor4 dot(0.6f, 0.6f, 0.6f, 1.f);
    if (sD.state == State::LISTENING)
    {
        text = llformat("Listening... %.1f s  (release to transcribe)", LLSpeechToText::captureSeconds());
        // A slow red pulse: the mic is live.
        const F32 a = 0.65f + 0.35f * std::sin((F32)now() * 6.f);
        dot = LLColor4(1.f, 0.2f, 0.2f, a);
    }
    else if (sD.state == State::TRANSCRIBING)
    {
        text = "Transcribing...";
        dot = LLColor4(1.f, 0.8f, 0.2f, 1.f);
    }
    else if (downloading)
    {
        text = llformat("Downloading the speech model... %d%%", (S32)(LLSpeechToText::downloadProgress() * 100.f));
        dot = LLColor4(0.35f, 0.85f, 1.f, 1.f);
    }
    else if (now() < sD.hintUntil)
    {
        text = sD.hint;
    }
    else
    {
        sD.hintUntil = 0.0;
        sD.hint.clear();
        return;
    }

    // Above the field being dictated into (while it still has focus), else low in the middle.
    S32 x = gViewerWindow->getWindowWidthScaled() / 2 - 120, y = 60;
    LLFocusableElement* focus = gFocusMgr.getKeyboardFocus();
    if (sD.state != State::IDLE && sD.target == Target::FIELD && focus && focus == sD.field)
    {
        if (LLUICtrl* ctrl = textTarget(focus))
        {
            const LLRect r = ctrl->calcScreenRect();
            x = r.mLeft;
            y = r.mTop + 4;
        }
    }
    // Scaled UI coordinates, as LLVRInput::drawCursor (this runs outside any UI scale).
    const LLVector2& ds = gViewerWindow->getDisplayScale();
    gGL.pushUIMatrix();
    gGL.scaleUI(ds.mV[VX], ds.mV[VY], 1.f);
    drawPill(x, y, text, dot);
    gGL.flush();
    gGL.popUIMatrix();
}

bool LLVRDictation::sendToNearbyChat(const std::string& text)
{
    std::string utf8 = text;
    LLStringUtil::trim(utf8);
    while (!utf8.empty() && utf8.front() == '/') utf8.erase(0, 1);
    LLStringUtil::trim(utf8);
    if (utf8.empty()) return false;
    static LLCachedControl<bool> s_anim(gSavedSettings, "PlayChatAnim", true);
    LLFloaterIMNearbyChat::sendChatFromViewer(utf8, CHAT_TYPE_NORMAL, s_anim);
    return true;
}
