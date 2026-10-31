/**
 * @file gmdxtpfx.cpp
 * @brief GunMetal-DX: teleport effects (arming, capture, timing, the hold; the effects are gmdxtpfx*.cpp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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
 * $/LicenseInfo$
 */

// <GMDX> whole file: see gmdxtpfx.h (the hooks) and gmdxtpfxeffect.h (one effect).
//
// How it works:
//  * CAPTURE. arm() / preview() ask the Vulkan renderer for a copy of the next frame's world + HUD
//    attachments (capture point 2, the same copy a UI snapshot uses: no UI in it). The next idle
//    reads it back once (a few ms) into a local texture, the effect's "frozen" frame.
//  * DRAW. LLHUDView::draw (over the world and its HUD-view extras, under every floater, the menu
//    bar and the toolbars, which stay live) calls the running effect's depart / hold / arrive in the
//    root view's coordinates over the whole window, then the hold's line and Cancel.
//  * STATE. The idle tick follows gAgent's teleport state.
//    Cross-region: the agent standing in another region (START_ARRIVAL / ARRIVING / NONE) and a few
//    frames drawn starts the arrival; NONE in the old region (failed, cancelled, or same-region after
//    all) fades out at once; the hold timeout, a window resize, a capture that never comes, logout or
//    a disconnect end it (the stock progress window is shown if the teleport is still under way).
//    Same-region (GMDXTPFXSameRegion, quick effects only, never where the agent can take damage):
//    TELEPORT_LOCAL starts a short departure; the arrival starts once the agent has moved (LOCAL ->
//    NONE) or QUICK_ARRIVE_BY at the latest; all told QUICK_ARRIVE_BY + QUICK_ARRIVE at most.
//  * IN THE WORLD (Effect::inWorld, gmdxtpfxworld.h): the effect's tick() starts its particles and
//    light; the 2D part only veils the screen into the hold and out of it, and hidesSelf() hides the
//    own avatar for the moments the effect asks. Its same-region variant waits for the real jump
//    (QUICK_WORLD_BY at the latest) and arrives over QUICK_WORLD_ARRIVE. The particles outlive the
//    run (smoke left behind keeps rolling); on logout or a disconnect they go at once.

#include "llviewerprecompiledheaders.h"

#include "gmdxtpfx.h"
#include "gmdxtpfxeffect.h"
#include "gmdxtpfxworld.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llapp.h"
#include "llaudioengine.h"
#include "llcallbacklist.h"
#include "lldir.h"
#include "llfloaterprogressview.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llhudview.h"
#include "llimage.h"
#include "llpanel.h"
#include "llrand.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llstartup.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewerdisplay.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "llvoavatarself.h"
#include "vkgfx/llvulkancontext.h"

#include <fstream>
#include <iterator>
#include <map>
#include <utility>

extern bool gDisconnected;

// ---- the effects: one X(...) line each, in the order Random sees them (the first is the fallback) ----
#define GMDXTPFX_EFFECTS(X) \
    X(tpfxShatter)          \
    X(tpfxNinjaSmoke)       \
    X(tpfxSmokePuff)        \
    X(tpfxLightning)        \
    X(tpfxCRT)              \
    X(tpfxPortal)           \
    X(tpfxGlitch)           \
    X(tpfxFold)             \
    X(tpfxHyperspace)       \
    /* a new effect: one X(...) line above this one */

#define GMDXTPFX_DECLARE(f) GMDXTPFX::Effect& f();
GMDXTPFX_EFFECTS(GMDXTPFX_DECLARE)
#undef GMDXTPFX_DECLARE

namespace
{
const std::vector<GMDXTPFX::Effect*>& registry()
{
#define GMDXTPFX_ENTRY(f) &f(),
    static const std::vector<GMDXTPFX::Effect*> all{ GMDXTPFX_EFFECTS(GMDXTPFX_ENTRY) };
#undef GMDXTPFX_ENTRY
    return all;
}

// ---- tunables (simple constants; the user-facing ones are GMDXTPFX* settings) ----
constexpr F64 ARM_SECS = 2.0;            // a user request arms the effect this long
constexpr U32 CAPTURE_FRAMES = 20;       // frames to wait for the capture before giving up
constexpr U32 QUICK_CAPTURE_FRAMES = 6;  // ... for a same-region one (the jump is near-instant)
constexpr U32 ARRIVED_FRAMES = 3;        // frames drawn in the new region before the arrival
constexpr F32 FADE_SECS = 0.25f;         // a failed / cancelled / timed-out effect fades this fast
constexpr F32 ARRIVE_MAX = 1.5f;         // the arrival never takes longer
constexpr F32 PREVIEW_HOLD = 1.0f;       // Preview: the hold between depart and arrive
constexpr F32 QUICK_DEPART = 0.25f;      // same-region: the departure
constexpr F32 QUICK_ARRIVE_BY = 0.35f;   // ... the arrival starts by then, moved or not
constexpr F32 QUICK_ARRIVE = 0.25f;      // ... and takes this long
constexpr F32 QUICK_WORLD_BY = 1.2f;     // a world effect waits this long for the jump (a server round trip)
constexpr F32 QUICK_WORLD_ARRIVE = 0.6f; // ... and arrives over this (its smoke has to swallow him first)
constexpr F32 QUICK_WORLD_PREVIEW = 0.6f;   // its preview "jumps" (in place) after this
constexpr S32 MAX_CAPTURE_W = 2048;      // larger captures are halved (memory; nobody sees the detail)
const LLColor4 HOLD_BG(0.035f, 0.04f, 0.05f, 1.f);
const LLColor4 HOLD_GLOW(0.10f, 0.12f, 0.15f, 1.f);

enum class Phase { Off, Capture, Run };

Phase sPhase = Phase::Off;
bool sPreview = false;
bool sQuick = false;            // the same-region variant
GMDXTPFX::Effect* sEffect = nullptr;
F64 sArmedAt = -1.0;
F64 sStart = 0.0, sArriveAt = -1.0, sOutAt = -1.0;
U32 sFrames = 0;                // Capture: frames waited; Run: frames drawn in the new region
U64 sRegion = 0;                // the region handle the teleport left
S32 sWinW = 0, sWinH = 0;       // raw window size at the start (a resize ends the effect)
F32 sW = 0.f, sH = 0.f;         // root-view (UI-scaled) window size
F32 sDur = 1.f, sIntensity = 1.f;
LLVector2 sAvatar;
LLVector3d sFrom;               // the agent when the teleport was asked for / the preview started
LLPointer<LLViewerTexture> sTex;
bool sCancelShown = false;
LLRect sCancelRect;             // root-view coordinates
std::function<void()> sPreviewDone;   // preview(): its "it is over"

F64 now() { return LLFrameTimer::getElapsedSeconds(); }

bool inFlight(LLAgent::ETeleportState s)
{
    return s == LLAgent::TELEPORT_PENDING || (s >= LLAgent::TELEPORT_START && s <= LLAgent::TELEPORT_MOVING);
}

// The parcel the agent stands on (or its region) allows damage: no same-region effect, ever.
bool damageHere()
{
    LLViewerParcelMgr* pm = LLViewerParcelMgr::getInstance();
    return pm->allowAgentDamage(gAgent.getRegion(), pm->getAgentParcel());
}

// The agent's avatar on screen (root-view pixels); the screen centre when it is not to be seen.
LLVector2 avatarOnScreen()
{
    LLCoordGL s;
    if (isAgentAvatarValid() && !gAgentCamera.isHidingAvatarForFirstPerson() &&
        LLViewerCamera::getInstance()->projectPosAgentToScreen(gAgentAvatarp->getPositionAgent(), s, false))
        return LLVector2((F32)s.mX, (F32)s.mY);
    return LLVector2(sW * 0.5f, sH * 0.5f);
}

// want: a name, "random", or (settings from before the effects had names) "0" Shatter / "1" Random.
GMDXTPFX::Effect* pick(const std::string& want, bool quick)
{
    std::vector<GMDXTPFX::Effect*> ok;
    for (GMDXTPFX::Effect* e : registry())
        if (quick ? e->quick() : e->crossRegion()) ok.push_back(e);
    if (ok.empty()) return nullptr;
    if (want == "random" || want == "1") return ok[ll_rand((S32)ok.size())];
    for (GMDXTPFX::Effect* e : ok)
        if (want == e->name()) return e;
    return ok.front();
}

// ---- sounds: app_settings/sounds/teleport/*.wav (scripts/tools/gmdx_tpfx_sounds.py) ----

// 16-bit PCM mono WAV (the files the script writes); anything else: false.
bool loadWav(const std::string& path, std::vector<S16>& pcm, U32& rate)
{
    std::ifstream f(path, std::ios::binary);
    if (!f.is_open()) return false;
    const std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (d.size() < 12 || d.compare(0, 4, "RIFF") || d.compare(8, 4, "WAVE")) return false;
    auto u16 = [&d](size_t o) { return (U32)(U8)d[o] | ((U32)(U8)d[o + 1] << 8); };
    auto u32 = [&u16](size_t o) { return u16(o) | (u16(o + 2) << 16); };
    U32 channels = 0, bits = 0;
    rate = 0;
    for (size_t o = 12; o + 8 <= d.size();)
    {
        const U32 len = u32(o + 4);
        const size_t body = o + 8;
        if (body + len > d.size()) return false;
        if (!d.compare(o, 4, "fmt ") && len >= 16)
        {
            if (u16(body) != 1) return false;   // PCM only
            channels = u16(body + 2);
            rate = u32(body + 4);
            bits = u16(body + 14);
        }
        else if (!d.compare(o, 4, "data"))
        {
            if (channels != 1 || bits != 16 || !rate) return false;
            pcm.resize(len / 2);
            memcpy(pcm.data(), d.data() + body, pcm.size() * 2);
            return !pcm.empty();
        }
        o = body + len + (len & 1);
    }
    return false;
}

struct Shot
{
    U32 id = 0;
    LLAudioEngine* engine = nullptr;   // handles belong to the engine that made them
    bool tried = false;
};

void play(const char* file)
{
    static LLCachedControl<bool> on(gSavedSettings, "GMDXTPFXSounds", false);
    static LLCachedControl<F32> volume(gSavedSettings, "GMDXTPFXVolume", 0.5f);
    static std::map<std::string, Shot> shots;
    if (!file || !on || !gAudiop) return;
    Shot& s = shots[file];
    if (s.engine != gAudiop)
    {
        s.engine = gAudiop;
        s.id = 0;
        s.tried = false;
    }
    if (!s.tried)
    {
        s.tried = true;
        std::vector<S16> pcm;
        U32 rate = 0;
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "sounds", "teleport", file);
        if (loadWav(path, pcm, rate)) s.id = gAudiop->createLocalShot(pcm.data(), (U32)pcm.size(), 1, rate);
        if (!s.id) LL_WARNS("GMDXTPFX") << "teleport sound not available: " << path << LL_ENDL;
    }
    if (!s.id) return;
    const F32 gain = llclamp((F32)volume, 0.f, 1.f) * gAudiop->getSecondaryGain(LLAudioEngine::AUDIO_TYPE_UI);
    gAudiop->setLocalShot(s.id, LLVector3::zero, gain, 1.f, 1.f, 0.f);
    gAudiop->playLocalShot(s.id, LLVector3::zero, gain, 1.f);
}

// ---- lifetime ----

void stop()
{
    const bool was_preview = sPreview;
    if (sEffect) sEffect->end();
    sEffect = nullptr;
    sPhase = Phase::Off;
    sPreview = sQuick = false;
    sTex = nullptr;
    sCancelShown = false;
    // A teleport still under way gets the stock progress window back (timeout, resize, capture
    // problem, a preview that ran into a real teleport).
    if (inFlight(gAgent.getTeleportState()))
    {
        if (LLFloaterProgressView* p = LLFloaterReg::getTypedInstance<LLFloaterProgressView>("progress_view"))
            p->setShowProgress(true);
    }
    LL_DEBUGS("GMDXTPFX") << "effect ended" << (was_preview ? " (preview)" : "") << LL_ENDL;
    if (std::function<void()> done = std::exchange(sPreviewDone, nullptr)) done();
}

// request: ask for a capture now (a teleport's was asked for in arm(), the frame before it began)
void startCapture(bool preview, bool quick, bool request)
{
    if (request && !LLVulkanContext::instance().requestCapture(2)) return;   // world + HUD attachments, no UI
    sPhase = Phase::Capture;
    sPreview = preview;
    sQuick = quick;
    sFrames = 0;
    sRegion = gAgent.getRegion() ? gAgent.getRegion()->getHandle() : 0;
}

void startArrival(F64 t_now)
{
    sArriveAt = t_now;
    sAvatar = avatarOnScreen();
    play(sEffect->arriveSound());
}

bool inWorld() { return sEffect && sEffect->inWorld(); }

// How long the arrival takes.
F32 arriveLen()
{
    if (sQuick) return inWorld() ? QUICK_WORLD_ARRIVE : QUICK_ARRIVE;
    return llmin(ARRIVE_MAX, 0.8f * sDur);
}

GMDXTPFX::Frame frame(F64 t_now)
{
    GMDXTPFX::Frame f;
    f.t = (F32)(t_now - sStart);
    f.u = llclamp(f.t / sDur, 0.f, 1.f);
    f.a = sArriveAt >= 0.0 ? llclamp((F32)(t_now - sArriveAt) / arriveLen(), 0.f, 1.f) : 0.f;
    f.alpha = sOutAt >= 0.0 ? 1.f - llclamp((F32)(t_now - sOutAt) / FADE_SECS, 0.f, 1.f) : 1.f;
    f.w = sW;
    f.h = sH;
    f.intensity = sIntensity;
    f.quick = sQuick;
    f.avatar = sAvatar;
    f.frozen = sTex.get();
    f.arriving = sArriveAt >= 0.0;
    f.left = !sPreview && !sQuick && gAgent.getRegion() && gAgent.getRegion()->getHandle() != sRegion;
    f.from = sFrom;
    return f;
}

// The capture is in: the texture, the effect, go.
bool begin()
{
    static LLCachedControl<std::string> effect_ctl(gSavedSettings, "GMDXTPFXEffect", "shatter");
    static LLCachedControl<std::string> quick_ctl(gSavedSettings, "GMDXTPFXSameRegionEffect", "ninjasmoke");
    static LLCachedControl<std::string> preview_ctl(gSavedSettings, "GMDXTPFXPreviewEffect", "");
    std::string want = sQuick ? (std::string)quick_ctl : (std::string)effect_ctl;
    if (sPreview && !((std::string)preview_ctl).empty()) want = preview_ctl;
    // (a preview of a same-region-only effect plays its same-region version)
    if (sPreview && !sQuick)
        for (GMDXTPFX::Effect* e : registry())
            if (want == e->name() && !e->crossRegion()) sQuick = true;
    GMDXTPFX::Effect* effect = pick(want, sQuick);
    if (!effect)
    {
        stop();   // (same-region, and no effect has a quick variant)
        return true;
    }

    std::vector<U8> rgb;
    U32 cw = 0, ch = 0;
    if (!LLVulkanContext::instance().readCapture(rgb, cw, ch) || !cw || !ch || cw > 65535 || ch > 65535) return false;
    LLPointer<LLImageRaw> raw = new LLImageRaw((U16)cw, (U16)ch, 3);
    if (raw->isBufferInvalid()) return false;
    memcpy(raw->getData(), rgb.data(), (size_t)cw * ch * 3);
    std::vector<U8>().swap(rgb);
    if ((S32)cw > MAX_CAPTURE_W) raw->scale((S32)cw / 2, (S32)ch / 2);
    sTex = LLViewerTextureManager::getLocalTexture(raw.get(), false);
    if (sTex.isNull()) return false;
    sTex->dontDiscard();
    sTex->setAddressMode(LLTexUnit::TAM_CLAMP);

    static LLCachedControl<F32> duration(gSavedSettings, "GMDXTPFXDuration", 1.0f);
    static LLCachedControl<F32> intensity(gSavedSettings, "GMDXTPFXIntensity", 1.0f);
    sEffect = effect;
    sDur = sQuick ? QUICK_DEPART : llclamp((F32)duration * effect->departSecs(), 0.3f, 3.f);
    sIntensity = llclamp((F32)intensity, 0.f, 3.f);
    const LLRect win = gViewerWindow->getWindowRectScaled();
    sW = (F32)win.getWidth();
    sH = (F32)win.getHeight();
    sWinW = gViewerWindow->getWindowWidthRaw();
    sWinH = gViewerWindow->getWindowHeightRaw();
    sAvatar = avatarOnScreen();
    sStart = now();
    sArriveAt = sOutAt = -1.0;
    sFrames = 0;
    sPhase = Phase::Run;
    sEffect->begin(frame(sStart));
    play(sEffect->departSound());
    LL_INFOS("GMDXTPFX") << (sPreview ? "preview" : "teleport") << ": " << sEffect->name() << (sQuick ? " (same region)" : "")
                         << ", " << cw << "x" << ch << " capture" << LL_ENDL;
    return true;
}

// The same-region variant, once running: never more than about half a second, never on a damage parcel.
void tickQuick(F64 t_now, F32 t, LLAgent::ETeleportState state)
{
    if (!sPreview && damageHere()) { stop(); return; }   // (the destination's parcel data can land mid-effect)
    if (sArriveAt >= 0.0)
    {
        if (t_now - sArriveAt >= arriveLen()) stop();
        return;
    }
    const bool world = inWorld();
    const bool moved = sPreview ? t >= (world ? QUICK_WORLD_PREVIEW : QUICK_DEPART) : state != LLAgent::TELEPORT_LOCAL;
    if ((moved && t >= sDur) || t >= (world ? QUICK_WORLD_BY : QUICK_ARRIVE_BY)) startArrival(t_now);
}

void step()
{
    static LLCachedControl<bool> preview_ctl(gSavedSettings, "GMDXTPFXPreview", false);
    static LLCachedControl<bool> preview_quick(gSavedSettings, "GMDXTPFXPreviewSameRegion", false);
    static LLCachedControl<bool> same_region(gSavedSettings, "GMDXTPFXSameRegion", false);
    if (preview_ctl)
    {
        gSavedSettings.setBOOL("GMDXTPFXPreview", false);
        GMDXTPFX::preview(preview_quick);
    }
    const bool live = !LLApp::isExiting() && !gDisconnected && LLStartUp::getStartupState() >= STATE_STARTED;
    if (!live)
    {
        sArmedAt = -1.0;
        if (sPhase != Phase::Off) stop();
        GMDXTPFX::World::clear();
        return;
    }
    static LLCachedControl<bool> enabled(gSavedSettings, "GMDXTPFXEnabled", false);
    static bool warmed = false;
    if (enabled && !warmed)
    {
        warmed = true;
        GMDXTPFX::World::warm();   // once in the world: the first effect's textures and blend pipeline are ready
    }
    const F64 t_now = now();
    const LLAgent::ETeleportState state = gAgent.getTeleportState();
    if (sPhase == Phase::Off)
    {
        if (sArmedAt < 0.0) return;
        if (t_now - sArmedAt > ARM_SECS)
            sArmedAt = -1.0;   // the request never started
        else if (state == LLAgent::TELEPORT_LOCAL)
        {
            sArmedAt = -1.0;   // same region: a quick effect, if he wants them and nobody can be hurt here
            if (same_region && !damageHere()) startCapture(false, true, false);
        }
        else if (state != LLAgent::TELEPORT_NONE)
        {
            sArmedAt = -1.0;
            startCapture(false, false, false);
        }
        return;
    }
    if (sPhase == Phase::Capture)
    {
        if (!sPreview && !sQuick && !inFlight(state)) { stop(); return; }   // over before it began
        if (begin()) return;
        if (++sFrames > (sQuick ? QUICK_CAPTURE_FRAMES : CAPTURE_FRAMES))
        {
            LL_WARNS("GMDXTPFX") << "no frame capture; teleport effect skipped" << LL_ENDL;
            stop();
        }
        return;
    }

    // Run
    static LLCachedControl<F32> timeout(gSavedSettings, "GMDXTPFXHoldTimeoutSec", 20.f);
    const F32 t = (F32)(t_now - sStart);
    if (gViewerWindow->getWindowWidthRaw() != sWinW || gViewerWindow->getWindowHeightRaw() != sWinH ||
        t > (sQuick ? (inWorld() ? 3.f : 2.f) : llmax((F32)timeout, 1.f) + 5.f))   // (the last one: a safety net, nothing should get here)
    {
        stop();
        return;
    }
    if (sQuick)
    {
        tickQuick(t_now, t, state);
        return;
    }
    if (sOutAt >= 0.0)
    {
        if (t_now - sOutAt >= FADE_SECS) stop();
        return;
    }
    if (sArriveAt >= 0.0)
    {
        if (t_now - sArriveAt >= arriveLen() && t >= sDur) stop();
        return;
    }
    if (sPreview)
    {
        if (t >= sDur + PREVIEW_HOLD) startArrival(t_now);
        return;
    }
    const LLViewerRegion* region = gAgent.getRegion();
    // Only once the teleport is over (TELEPORT_NONE): through START_ARRIVAL / ARRIVING (the stock
    // TeleportArrivalDelay, 2 s) idle() returns early, so the camera and objects are not updated and
    // the frames still show the old place. The stock progress screen covers that wait; so must we.
    const bool landed = region && region->getHandle() != sRegion && state == LLAgent::TELEPORT_NONE;
    if (landed)
    {
        // the new region is rendering: a few frames, and not before the departure has been seen
        if (++sFrames >= ARRIVED_FRAMES && t >= sEffect->arriveFrom() * sDur) startArrival(t_now);
    }
    else if (state == LLAgent::TELEPORT_NONE || state == LLAgent::TELEPORT_LOCAL)
        sOutAt = t_now;   // failed, cancelled, or it was a same-region teleport after all
    else if (t >= (F32)timeout)
        sOutAt = t_now;   // the stock progress window takes over (stop())
}

void tick(void*)
{
    step();
    if (sPhase == Phase::Run && sEffect) sEffect->tick(frame(now()));
    // The world effects' particles, on the frames idle() skips the particle update (a teleport under
    // way: it returns before LLWorld::updateParticles); the others feed from there (feedParticles).
    if (gTeleportDisplay) GMDXTPFX::feedParticles();
}

void ensureIdle()
{
    static bool added = false;
    if (!added) gIdleCallbacks.addFunction(tick, nullptr);
    added = true;
}

// ---- the hold's line and Cancel (root-view coordinates) ----

void drawHold(F32 t, F32 alpha)
{
    static LLCachedControl<F32> cancel_after(gSavedSettings, "GMDXTPFXHoldCancelSec", 3.f);
    const LLColor4 text(0.72f, 0.76f, 0.8f, 0.9f * alpha);
    const F32 y = sH * 0.2f;
    LLFontGL::getFontSansSerif()->renderUTF8(llformat("Teleporting\xE2\x80\xA6 %d s", (S32)t), 0, sW * 0.5f, y, text,
                                             LLFontGL::HCENTER, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
    // Cancel, when the teleport can be cancelled (the stock window's own button says so)
    bool can_cancel = !sPreview && t >= (F32)cancel_after;
    if (can_cancel)
    {
        LLFloaterProgressView* p = LLFloaterReg::findTypedInstance<LLFloaterProgressView>("progress_view");
        LLView* btn = p ? p->findChildView("cancel_btn") : nullptr;
        can_cancel = btn && btn->getVisible() && btn->getEnabled();
    }
    sCancelShown = can_cancel;
    if (!can_cancel) return;
    constexpr S32 BW = 96, BH = 24;
    sCancelRect.setOriginAndSize((S32)(sW * 0.5f) - BW / 2, (S32)y - 22 - BH, BW, BH);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gl_rect_2d(sCancelRect, LLColor4(0.12f, 0.13f, 0.15f, 0.85f * alpha), true);
    gl_rect_2d(sCancelRect, LLColor4(0.5f, 0.55f, 0.6f, 0.7f * alpha), false);
    LLFontGL::getFontSansSerif()->renderUTF8("Cancel", 0, (F32)sCancelRect.getCenterX(), (F32)sCancelRect.getCenterY(),
                                             LLColor4(0.85f, 0.88f, 0.92f, alpha), LLFontGL::HCENTER, LLFontGL::VCENTER);
}
} // namespace

// ---- shared helpers (gmdxtpfxeffect.h) ----

F32 GMDXTPFX::smooth(F32 a, F32 b, F32 x) { const F32 t = llclamp((x - a) / (b - a), 0.f, 1.f); return t * t * (3.f - 2.f * t); }
F32 GMDXTPFX::rnd(F32 a, F32 b) { return a + (b - a) * ll_frand(); }

void GMDXTPFX::Effect::hold(const Frame& f) { drawCover(f, 0.f); }

void GMDXTPFX::veil(const Frame& f, F32 k)
{
    Frame g = f;
    g.alpha = f.alpha * llclamp(k, 0.f, 1.f);
    if (g.alpha > 0.004f) drawCover(g, 0.f);
}

void GMDXTPFX::fill(const Frame& f, const LLColor4& c)
{
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gl_rect_2d(0, (S32)f.h + 1, (S32)f.w + 1, 0, c, true);
}

void GMDXTPFX::drawCover(const Frame& f, F32 reveal)
{
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    LLColor4 bg = HOLD_BG;
    bg.mV[VALPHA] = f.alpha;
    const F32 diag = sqrtf(f.w * f.w + f.h * f.h);
    if (reveal <= 0.f) gl_rect_2d(0, (S32)f.h + 1, (S32)f.w + 1, 0, bg, true);
    gGL.pushUIMatrix();
    gGL.translateUI(f.w * 0.5f, f.h * 0.5f, 0.f);
    if (reveal <= 0.f)
    {
        LLColor4 glow = HOLD_GLOW;
        glow.mV[VALPHA] = f.alpha * (0.30f + 0.06f * sinf(f.t * 2.f));   // a slow, faint breathing
        LLColor4 clear = glow;
        clear.mV[VALPHA] = 0.f;
        gl_washer_2d(0.6f * f.h, 0.f, 48, glow, clear);
    }
    else
    {
        // the iris: clear inside r, a soft band, the hold outside; a faint ring on the edge
        const F32 band = 0.22f * diag;
        const F32 r = (0.5f * diag + band) * reveal;
        LLColor4 clear = bg;
        clear.mV[VALPHA] = 0.f;
        gl_washer_2d(r, llmax(0.f, r - band), 64, clear, bg);
        gl_washer_2d(diag + band, r, 64, bg, bg);
        gl_washer_2d(r + 1.5f, llmax(0.f, r - 1.5f), 64, LLColor4(0.8f, 0.9f, 1.f, 0.f),
                     LLColor4(0.8f, 0.9f, 1.f, 0.18f * f.alpha * (1.f - reveal)));
    }
    gGL.popUIMatrix();
}

void GMDXTPFX::drawFrozen(const Frame& f, const LLColor4& c, F32 x0, F32 y0, F32 x1, F32 y1, F32 u0, F32 v0, F32 u1, F32 v1)
{
    if (!f.frozen) return;
    gGL.getTexUnit(0)->bind(f.frozen);
    gGL.color4fv(c.mV);
    gGL.begin(LLRender::TRIANGLES);
    gGL.texCoord2f(u0, v0); gGL.vertex2f(x0, y0);
    gGL.texCoord2f(u1, v0); gGL.vertex2f(x1, y0);
    gGL.texCoord2f(u1, v1); gGL.vertex2f(x1, y1);
    gGL.texCoord2f(u0, v0); gGL.vertex2f(x0, y0);
    gGL.texCoord2f(u1, v1); gGL.vertex2f(x1, y1);
    gGL.texCoord2f(u0, v1); gGL.vertex2f(x0, y1);
    gGL.end();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}

F32 GMDXTPFX::farCorner(const Frame& f, const LLVector2& p)
{
    const F32 dx = llmax(p.mV[VX], f.w - p.mV[VX]), dy = llmax(p.mV[VY], f.h - p.mV[VY]);
    return sqrtf(dx * dx + dy * dy);
}

void GMDXTPFX::drawFrozenPolar(const Frame& f, const LLVector2& c, F32 rmax, S32 rings, S32 sectors, const Warp& warp)
{
    if (!f.frozen || rings < 1 || sectors < 3 || f.w <= 0.f || f.h <= 0.f) return;
    struct V { LLVector2 p, uv; LLColor4 c; };
    const S32 row = sectors + 1;
    std::vector<V> v((size_t)(rings + 1) * row);
    for (S32 i = 0; i <= rings; ++i)
    {
        const F32 r = rmax * powf((F32)i / (F32)rings, 1.5f);   // crowded toward the centre (the twist lives there)
        for (S32 j = 0; j <= sectors; ++j)
        {
            const F32 ang = F_TWO_PI * (F32)j / (F32)sectors;
            V& q = v[(size_t)i * row + j];
            q.uv.set((c.mV[VX] + r * cosf(ang)) / f.w, (c.mV[VY] + r * sinf(ang)) / f.h);
            q.p = c + LLVector2(cosf(ang), sinf(ang)) * r;
            q.c = LLColor4(1.f, 1.f, 1.f, f.alpha);
            warp(r, ang, q.p, q.c);
            const F32 out = llmax(0.f, llmax(llmax(-q.uv.mV[VX], q.uv.mV[VX] - 1.f), llmax(-q.uv.mV[VY], q.uv.mV[VY] - 1.f)));
            q.c.mV[VALPHA] *= 1.f - smooth(0.f, 0.06f, out);
        }
    }
    gGL.getTexUnit(0)->bind(f.frozen);
    gGL.begin(LLRender::TRIANGLES);
    for (S32 i = 0; i < rings; ++i)
        for (S32 j = 0; j < sectors; ++j)
        {
            const V* q[4] = { &v[(size_t)i * row + j], &v[(size_t)i * row + j + 1], &v[(size_t)(i + 1) * row + j + 1],
                              &v[(size_t)(i + 1) * row + j] };
            for (S32 k : { 0, 1, 2, 0, 2, 3 })
            {
                gGL.color4fv(q[k]->c.mV);
                gGL.texCoord2f(q[k]->uv.mV[VX], q[k]->uv.mV[VY]);
                gGL.vertex2f(q[k]->p.mV[VX], q[k]->p.mV[VY]);
            }
        }
    gGL.end();
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
}

// ---- the hooks (gmdxtpfx.h) ----

void GMDXTPFX::arm()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "GMDXTPFXEnabled", false);
    ensureIdle();
    if (!enabled) return;
    if (sPreview && sPhase != Phase::Off) stop();   // a real teleport takes over from a preview
    if (sPhase != Phase::Off) return;
    const LLAgent::ETeleportState state = gAgent.getTeleportState();
    if (state != LLAgent::TELEPORT_NONE && state != LLAgent::TELEPORT_PENDING) return;   // refused (teleportCore): one is under way
    sArmedAt = now();
    sFrom = gAgent.getPositionGlobal();
    LLVulkanContext::instance().requestCapture(2);   // the frame rendered next: the last one before departure
    GMDXTPFX::World::warm();   // (a world effect's textures in before its first frame)
}

void GMDXTPFX::disarm()
{
    sArmedAt = -1.0;
}

void GMDXTPFX::preview(bool same_region, std::function<void()> done)
{
    if (LLStartUp::getStartupState() < STATE_STARTED)
    {
        if (done) done();   // (no world before login, and no idle tick to end it)
        return;
    }
    ensureIdle();
    if (sPhase != Phase::Off) stop();
    sFrom = gAgent.getPositionGlobal();
    GMDXTPFX::World::warm();
    startCapture(true, same_region, true);
    if (sPhase != Phase::Off) sPreviewDone = std::move(done);
    else if (done) done();   // (no capture to be had: nothing plays)
}

bool GMDXTPFX::hidesProgress()
{
    return sArmedAt >= 0.0 || (sPhase != Phase::Off && !sPreview);
}

void GMDXTPFX::draw()
{
    ensureIdle();   // (the first UI frame registers the tick: GMDXTPFXPreview works from the start)
    if (sPhase != Phase::Run || !sEffect || sTex.isNull() || !gHUDView) return;
    const Frame f = frame(now());
    const LLRect hud = gHUDView->getRect();
    gGL.pushUIMatrix();
    gGL.translateUI(-(F32)hud.mLeft, -(F32)hud.mBottom, 0.f);   // root-view coordinates: the whole window
    const bool arriving = sArriveAt >= 0.0;
    if (arriving) sEffect->arrive(f);
    else if (f.u < 1.f) sEffect->depart(f);
    else sEffect->hold(f);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    sCancelShown = false;
    if (!sQuick && f.t >= sDur && !arriving && sOutAt < 0.0) drawHold(f.t, f.alpha);
    gGL.popUIMatrix();
}

bool GMDXTPFX::hidesSelf()
{
    return sPhase == Phase::Run && sEffect && sEffect->hidesAvatar(frame(now()));
}

bool GMDXTPFX::handleMouseDown(int x, int y)
{
    if (!sCancelShown || sPhase != Phase::Run || !gHUDView) return false;
    const LLRect hud = gHUDView->getRect();
    if (!sCancelRect.pointInRect(x + hud.mLeft, y + hud.mBottom)) return false;
    LL_INFOS("GMDXTPFX") << "teleport cancelled from the hold" << LL_ENDL;
    sCancelShown = false;
    gAgent.teleportCancel();   // -> TELEPORT_NONE: the tick fades the effect out
    return true;
}

// ---- Preferences > Teleport Effects (panel_preferences_tpfx.xml) ----

void GMDXTPFX::initPrefs(LLPanel* panel)
{
    // What is live: the whole page follows "Play teleport effects", except its note and the two
    // Previews (a preview plays with the effects off: that is how people try them). On top of that
    // the glass pattern only for Shatter (or Random, or an old / unknown name, which plays Shatter),
    // the same-region effect only with its checkbox, the volume only with the sounds.
    const LLHandle<LLPanel> handle = panel->getHandle();
    auto refresh = [handle]()
    {
        LLPanel* p = handle.get();
        if (!p) return;
        const bool on = gSavedSettings.getBOOL("GMDXTPFXEnabled");
        for (LLView* v : *p->getChildList()) v->setEnabled(on);
        for (const char* n : { "tpfx_enabled", "tpfx_note", "tpfx_preview_btn", "tpfx_preview_same_region_btn" })
            p->getChildView(n)->setEnabled(true);
        const std::string fx = gSavedSettings.getString("GMDXTPFXEffect");
        const GMDXTPFX::Effect* e = (fx == "random" || fx == "1") ? nullptr : pick(fx, false);
        const bool glass = on && (!e || !strcmp(e->name(), "shatter"));
        p->getChildView("tpfx_pattern_label")->setEnabled(glass);
        p->getChildView("tpfx_pattern_combo")->setEnabled(glass);
        const bool same = on && gSavedSettings.getBOOL("GMDXTPFXSameRegion");
        p->getChildView("tpfx_same_region_label")->setEnabled(same);
        p->getChildView("tpfx_same_region_combo")->setEnabled(same);
        p->getChildView("tpfx_volume")->setEnabled(on && gSavedSettings.getBOOL("GMDXTPFXSounds"));
    };
    // Settings, not commits: Cancel, Debug Settings and the like change them too.
    static std::vector<boost::signals2::scoped_connection> watch;   // (one panel at a time)
    watch.clear();
    for (const char* c : { "GMDXTPFXEnabled", "GMDXTPFXEffect", "GMDXTPFXSameRegion", "GMDXTPFXSounds" })
        watch.emplace_back(gSavedSettings.getControl(c)->getSignal()->connect(
            [refresh](LLControlVariable*, const LLSD&, const LLSD&) { refresh(); }));
    refresh();

    // The effect draws under the floaters: Preferences steps aside while the preview plays.
    for (const bool same_region : { false, true })
        panel->getChild<LLUICtrl>(same_region ? "tpfx_preview_same_region_btn" : "tpfx_preview_btn")
            ->setCommitCallback([handle, same_region](LLUICtrl*, const LLSD&)
            {
                LLFloater* floater = handle.get() ? handle.get()->getParentByType<LLFloater>() : nullptr;
                const LLHandle<LLFloater> fh = floater ? floater->getHandle() : LLHandle<LLFloater>();
                if (floater) floater->setVisible(false);
                GMDXTPFX::preview(same_region, [fh]()
                {
                    if (LLFloater* f = fh.get()) f->setVisibleAndFrontmost();
                });
            });
}
