/**
 * @file gmdxtpfx.h
 * @brief GunMetal-DX: teleport effects (the Shatter departure, a calm hold, the arrival reveal).
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

// <GMDX> Teleport effects (GMDXTPFX* settings, off by default; purely local and cosmetic).
// Plays ONLY on teleports the user starts: every LLAgent::teleportVia* call arms it
// (landmarks, Home, the world map, map / SLURL links, accepted offers...), and the script-driven
// callers of those (RLV @tpto / @standtp, RLV auto-accepted and god lures) disarm it right after.
// Teleports the server starts on its own (llTeleportAgent, experiences, death) never arm it.
// The request is never delayed: the departure plays over the frozen last frame while the server
// works, a calm hold covers the wait (Cancel after GMDXTPFXHoldCancelSec; the stock progress window
// takes over after GMDXTPFXHoldTimeoutSec or on any problem), and the new region is revealed once it
// renders. A failed or cancelled teleport fades the effect out at once. Same-region teleports play a
// short "quick" effect only with GMDXTPFXSameRegion on, and never where the agent can take damage.
// The effects themselves live in gmdxtpfx<name>.cpp (gmdxtpfxeffect.h). Ninja smoke, Smoke puff
// and Lightning happen in the world instead (local particles, a light, the own avatar hidden for
// a moment: gmdxtpfxworld.h); their departure plays in the live world while the server works.

#pragma once

#include <functional>

class LLPanel;

namespace GMDXTPFX
{
void arm();             // a user-started teleport request (LLAgent::teleportVia*, before the request)
void disarm();          // the request just made was script / RLV / god driven
// depart -> hold -> arrive in place, no teleport (Preferences; GMDXTPFXPreview). done: called once
// when it is over (or at once, when it cannot start).
void preview(bool same_region = false, std::function<void()> done = nullptr);
void initPrefs(LLPanel* panel);   // Preferences > Teleport Effects: what is live, the Previews (LLPanelPreference::postBuild)
bool hidesProgress();   // the stock progress window stays hidden (llviewerdisplay.cpp update_tp_display)
void draw();                              // over the world, under the floaters (LLHUDView::draw)
bool handleMouseDown(int x, int y);       // the hold's Cancel (LLHUDView coordinates)
bool hidesSelf();       // the own avatar is hidden in the main view (LLVOAvatar::selfHiddenInMainView)
// The world effects' particles (gmdxtpfxworld.cpp): LLViewerPartSim::updateSimulation, right after
// the frame's spawn counts are cleared (and the idle tick on the frames idle() skips that).
void feedParticles();
// Their light, when one is lit now (agent space, linear rgb, LL light radius): buildVulkanWorldFrame.
bool worldLight(float pos[3], float rgb[3], float& radius);
}
