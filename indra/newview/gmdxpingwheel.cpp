/**
 * @file gmdxpingwheel.cpp
 * @brief GunMetal-DX: the ping wheel and its keys in Preferences (see gmdxpingwheel.h).
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

// <GMDX> whole file: see gmdxpingwheel.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxpingwheel.h"

#include "gmdxmapshare.h"
#include "llagentcamera.h"
#include "llbutton.h"
#include "llcallbacklist.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llfocusmgr.h"
#include "llfontgl.h"
#include "llhudview.h"
#include "llkeyconflict.h"
#include "llnotificationsutil.h"
#include "llpanel.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llsetkeybinddialog.h"
#include "lltoolcomp.h"
#include "llviewercontrol.h"
#include "llviewerwindow.h"

namespace
{
enum class Phase { Idle, Pressed, Open };
Phase sPhase = Phase::Idle;
F64 sPressedAt = 0.0;
U32 sHeldFrame = 0;                     // the last frame its key arrived down or held
GMDXMapShare::PingTarget sTarget;       // where, from when the key went down
F32 sCentreX = 0.f, sCentreY = 0.f;     // the wheel's middle, in the HUD view
bool sFreedMouse = false;

constexpr F32 OUTER = 74.f, INNER = 26.f;   // px
constexpr F32 SLICE_MID[3] = { 90.f, 210.f, 330.f };   // degrees: Ping up, Enemy lower left, Rally lower right
constexpr U32 RELEASE_FRAMES = 3;       // frames without the key held: let go (its modifier went first)

void tick(void*);

void ensure_idle()
{
    static bool added = false;
    if (!added) gIdleCallbacks.addFunction(tick, nullptr);
    added = true;
}

// The pointer in the HUD view.
void mouse_local(F32& x, F32& y)
{
    S32 lx = 0, ly = 0;
    if (gHUDView) gHUDView->screenPointToLocal(gViewerWindow->getCurrentMouseX(), gViewerWindow->getCurrentMouseY(), &lx, &ly);
    x = (F32)lx;
    y = (F32)ly;
}

// The slice under the pointer: 0 Ping, 1 Enemy, 2 Rally; -1 the middle (nothing).
S32 hovered()
{
    F32 x, y;
    mouse_local(x, y);
    const F32 dx = x - sCentreX, dy = y - sCentreY;
    if (dx * dx + dy * dy < INNER * INNER) return -1;
    F32 angle = atan2f(dy, dx) * RAD_TO_DEG;
    if (angle < 0.f) angle += 360.f;
    S32 best = 0;
    F32 best_d = 360.f;
    for (S32 k = 0; k < 3; ++k)
    {
        F32 d = fabsf(angle - SLICE_MID[k]);
        d = llmin(d, 360.f - d);
        if (d < best_d) { best_d = d; best = k; }
    }
    return best;
}

void open_wheel()
{
    // mouselook (and OTS) pin and hide the cursor: free it, as Quick Swap does
    sFreedMouse = gAgentCamera.cameraMouselook() && !LLToolCompGun::getInstance()->isMouseFreed();
    if (sFreedMouse) LLToolCompGun::getInstance()->freeMouseForUI();
    S32 x = 0, y = 0;
    if (gHUDView)
    {
        if (gAgentCamera.cameraMouselook())
        {
            const LLRect world = gViewerWindow->getWorldViewRectScaled();
            gHUDView->screenPointToLocal(world.getCenterX(), world.getCenterY(), &x, &y);
        }
        else gHUDView->screenPointToLocal(gViewerWindow->getCurrentMouseX(), gViewerWindow->getCurrentMouseY(), &x, &y);
    }
    sCentreX = (F32)x;
    sCentreY = (F32)y;
    sPhase = Phase::Open;
}

void let_go()
{
    const Phase was = sPhase;
    sPhase = Phase::Idle;
    if (was == Phase::Pressed) GMDXMapShare::pingTo(sTarget, 0);   // a tap: a plain Ping
    else if (was == Phase::Open)
    {
        const S32 kind = hovered();
        if (sFreedMouse && gAgentCamera.cameraMouselook() && LLToolCompGun::getInstance()->isMouseFreed())
            LLToolCompGun::getInstance()->recaptureMouse();
        sFreedMouse = false;
        if (kind >= 0) GMDXMapShare::pingTo(sTarget, kind);
    }
}

void tick(void*)
{
    if (sPhase == Phase::Idle) return;
    if (LLFrameTimer::getFrameCount() - sHeldFrame >= RELEASE_FRAMES)
    {
        // the key stopped arriving without a release (window lost focus, a teleport screen, a
        // locked focus): nothing is sent; a real release (the modifier let go first too) sends
        if (!gViewerWindow->getActive() || gViewerWindow->getShowProgress() || gFocusMgr.focusLocked())
        {
            sPhase = Phase::Idle;
            if (sFreedMouse && gAgentCamera.cameraMouselook() && LLToolCompGun::getInstance()->isMouseFreed())
                LLToolCompGun::getInstance()->recaptureMouse();
            sFreedMouse = false;
            return;
        }
        let_go();
        return;
    }
    static LLCachedControl<S32> hold_ms(gSavedSettings, "GMDXPingWheelHoldMs", 250);
    if (sPhase == Phase::Pressed && LLFrameTimer::getTotalSeconds() - sPressedAt >= llclamp((S32)hold_ms, 50, 2000) / 1000.0)
        open_wheel();
}
} // namespace

void GMDXPingWheel::onKey(const LLKeyPressState& state)
{
    if (state.mState == KEYSTATE_UP)
    {
        if (sPhase != Phase::Idle) let_go();
        return;
    }
    if (state.mState == KEYSTATE_LEVEL)
    {
        if (sPhase != Phase::Idle) sHeldFrame = LLFrameTimer::getFrameCount();
        return;
    }
    // down: (as the ping keys, Alt / Ctrl with it is not a ping)
    if (sPhase != Phase::Idle || (gKeyboard->currentMask(false) & (MASK_ALT | MASK_CONTROL))) return;
    if (!GMDXMapShare::pingTarget(sTarget)) return;   // (nothing there: a tip said so)
    sPhase = Phase::Pressed;
    sPressedAt = LLFrameTimer::getTotalSeconds();
    sHeldFrame = LLFrameTimer::getFrameCount();
    ensure_idle();
}

void GMDXPingWheel::draw()
{
    if (sPhase != Phase::Open) return;
    static const char* const kLabels[3] = { "Ping", "Enemy", "Rally" };
    const S32 hover = hovered();
    const LLColor4 dark(0.05f, 0.06f, 0.07f, 0.62f), dark_edge(0.05f, 0.06f, 0.07f, 0.78f);
    const F32 gap = 2.5f * DEG_TO_RAD;
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    gGL.pushUIMatrix();
    gGL.translateUI(sCentreX, sCentreY, 0.f);
    for (S32 k = 0; k < 3; ++k)
    {
        const F32 from = (SLICE_MID[k] - 60.f) * DEG_TO_RAD + gap, to = (SLICE_MID[k] + 60.f) * DEG_TO_RAD - gap;
        LLColor4 c = GMDXMapShare::pingColor(k);
        if (k == hover)
        {
            LLColor4 inner = c, outer = c;
            inner.mV[VALPHA] = 0.38f;
            outer.mV[VALPHA] = 0.62f;
            gl_washer_segment_2d(OUTER, INNER, from, to, 24, inner, outer);
        }
        else gl_washer_segment_2d(OUTER, INNER, from, to, 24, dark, dark_edge);
        c.mV[VALPHA] = k == hover ? 1.f : 0.55f;   // a thin edge in the kind's colour
        gGL.color4fv(c.mV);
        gl_arc_2d(0.f, 0.f, OUTER, 24, false, from, to);
    }
    gGL.color4fv((hover < 0 ? LLColor4(0.2f, 0.22f, 0.24f, 0.75f) : dark).mV);
    gl_circle_2d(0.f, 0.f, INNER - 5.f, 24, true);
    gGL.popUIMatrix();

    const LLFontGL* font = LLFontGL::getFontSansSerifSmallBold();
    for (S32 k = 0; k < 3; ++k)
    {
        const F32 a = SLICE_MID[k] * DEG_TO_RAD, r = 0.5f * (OUTER + INNER);
        const LLColor4 c = k == hover ? LLColor4(0.97f, 0.98f, 0.99f, 1.f) : GMDXMapShare::pingColor(k);
        font->renderUTF8(kLabels[k], 0, sCentreX + r * cosf(a), sCentreY + r * sinf(a), c, LLFontGL::HCENTER, LLFontGL::VCENTER,
                         LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
    }
    LLFontGL::getFontSansSerifSmall()->renderUTF8("Cancel", 0, sCentreX, sCentreY,
                                                  LLColor4(0.8f, 0.82f, 0.85f, hover < 0 ? 1.f : 0.5f), LLFontGL::HCENTER,
                                                  LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
}

// ---- Preferences > Combat > Map Share: the ping keys ---------------------------------------------

namespace
{
// The ping keys, set in the modes the pings work in (as the defaults are: on foot in third person and
// mouselook, and sitting), with the viewer's key capture dialog; written to key_bindings.xml at once.
class GMDXPanelPingKeys : public LLPanel, public LLKeyBindResponderInterface
{
public:
    bool postBuild() override
    {
        for (const auto& [button, control] : kKeys)
        {
            const std::string name = control;
            childSetAction(button, [this, name](LLUICtrl*, const LLSD&) { edit(name); });
        }
        refresh();
        return true;
    }

    void onVisibilityChange(bool visible) override
    {
        LLPanel::onVisibilityChange(visible);
        if (visible) refresh();
    }

    void refresh() override
    {
        LLKeyConflictHandler handler(LLKeyConflictHandler::MODE_THIRD_PERSON);
        for (const auto& [button, control] : kKeys)
        {
            const std::string text = LLKeyConflictHandler::getStringFromKeyData(handler.getControl(control, 0));
            if (LLButton* b = findChild<LLButton>(button)) b->setLabel(text.empty() ? std::string("(none)") : text);
        }
    }

    void onCancelKeyBind() override {}

    void onDefaultKeyBind(bool) override
    {
        for (LLKeyConflictHandler::ESourceMode mode : kModes)
        {
            LLKeyConflictHandler handler(mode);
            handler.resetToDefault(mEditing, 0);
            handler.saveToSettings();
        }
        refresh();
    }

    bool onSetKeyBind(EMouseClickType click, KEY key, MASK mask, bool) override
    {
        bool failed = false;
        for (LLKeyConflictHandler::ESourceMode mode : kModes)
        {
            LLKeyConflictHandler handler(mode);
            if (click == CLICK_NONE && key == KEY_NONE) handler.clearControl(mEditing, 0);
            else if (!handler.registerControl(mEditing, 0, click, key, mask, true)) failed = true;
            handler.saveToSettings();
        }
        refresh();
        if (failed)
        {
            LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE",
                "That key is taken by a menu or kept by another control in some modes: see Preferences > Controls."));
        }
        return failed;
    }

private:
    void edit(const std::string& control)
    {
        LLSetKeyBindDialog* dialog = LLFloaterReg::getTypedInstance<LLSetKeyBindDialog>("keybind_dialog", LLSD());
        if (!dialog) return;
        mEditing = control;
        dialog->setParent(this, this, ALLOW_MOUSE | ALLOW_MASK_MOUSE | ALLOW_KEYS | ALLOW_MASKS | ALLOW_MASK_KEYS);
        if (LLFloater* root = gFloaterView->getParentFloater(this)) root->addDependentFloater(dialog);
        dialog->openFloater();
        dialog->setFocus(true);
    }

    static constexpr std::pair<const char*, const char*> kKeys[] = { { "ping_wheel_key", "map_ping_wheel" },
                                                                     { "ping_key", "map_ping" },
                                                                     { "ping_enemy_key", "map_ping_enemy" },
                                                                     { "ping_rally_key", "map_ping_rally" } };
    static constexpr LLKeyConflictHandler::ESourceMode kModes[] = { LLKeyConflictHandler::MODE_THIRD_PERSON,
                                                                   LLKeyConflictHandler::MODE_FIRST_PERSON,
                                                                   LLKeyConflictHandler::MODE_SITTING };
    std::string mEditing;
};

static LLPanelInjector<GMDXPanelPingKeys> t_gmdx_ping_keys("gmdx_ping_keys");
} // namespace
