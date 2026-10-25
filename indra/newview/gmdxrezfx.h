/**
 * @file gmdxrezfx.h
 * @brief GunMetal-DX rez effects: what objects do on screen when they appear and disappear.
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

// <GMDX> Rez effects (Preferences > Rez Effects). The Tron rez-in and the ember de-rez come
// from Vulkanstorm (SoapStorm's retired native-Vulkan fork); the rest are GunMetal-DX's.
//
// How it fits the Vulkan renderer: an object in an effect carries instance flag bits 23..31
// (its slot in a small effect table + 1) on every row of LLCleanInstanceStore, and bit 22
// while it is dying. The table (draw set 0 binding 45) says per slot which effect, which stage
// and how far along; this file rewrites it every frame while effects run, and nothing at all
// runs or uploads while none do. Objects that start the same frame with the same effect share
// a slot, so a whole region rezzing in at login needs a few dozen slots, not thousands.
//
// Fairness (Orion's rule for a combat viewer: nothing may hide information or give an
// advantage): rez-in effects never move a vertex, and never discard a fragment -- the object is
// drawn whole and solid from its first frame, the effect only paints its surface. The exception
// is the 3D printer and the transporter (Orion's call): they may hide what is not built yet, and
// that part cannot be clicked; with combat-safe on, on damage-enabled land, they too stay whole
// and solid throughout, paint only. A killed object
// keeps drawing for the de-rez alone: it casts no shadow, leaves reflection probes, ray
// tracing and the GPU pick, and is not in the object list (no clicks). Temp-on-rez objects
// (bullets, projectiles) never play any effect.

#pragma once

#include "lluuid.h"
#include "stdtypes.h"

#include <cstddef>
#include <cstdint>

class LLViewerObject;

namespace GMDXRezFx
{
    // Instance flag bits (LLCleanInstanceStore::REZFX_*; vkclean/shaders/gmdx_rezfx.glsl).
    constexpr U32 DYING_FLAG = 0x00400000u;   // bit 22
    constexpr U32 SLOT_SHIFT = 23u;           // bits 23..31: slot + 1
    constexpr U32 SLOT_MASK  = 0xFF800000u;
    constexpr U32 ALL_FLAGS  = DYING_FLAG | SLOT_MASK;
    constexpr U32 MAX_SLOTS  = 511u;

    // Shader effect ids (gmdx_rezfx.glsl). Rez-in below DEREZ_FIRST, de-rez from it.
    enum Effect : U32
    {
        FX_NONE = 0, FX_TRON = 1, FX_FIRE = 2, FX_HOLOGRAM = 3, FX_VOXEL = 4, FX_PRINTER = 5, FX_TRANSPORTER = 6,
        FX_EMBER = 8, FX_DISINTEGRATE = 9, FX_ASH = 10, FX_GLITCH = 11, FX_IMPLODE = 12, FX_TRON_OUT = 13, FX_SHATTER = 14,
        DEREZ_FIRST = FX_EMBER
    };

    // One table row, std430 (gmdx_rezfx.glsl RezFxSlot, where each field's per-effect meaning is).
    struct Slot
    {
        U32 effect;       // Effect (0: unused row)
        U32 stage;        // Tron: 0 the wireframe scan (holds at 1 while textures load), 1 the fill scan
                          // (the other rez-ins hold by progress: gmdxrezfx.cpp kWaitMax)
        F32 progress;     // 0..1 through the stage
        F32 time;         // seconds since the stage began (flicker / sparkle animation)
        F32 colour[4];    // linear rgb, a = 1 (Tron, Tron de-rez: glow strength)
        F32 wind[4];      // ash: unit drift direction (agent space), w = strength 0..1; the rest: z = length (s);
                          // rez-ins waiting for textures (GMDXRezFxWaitForTextures): w = the hold's idle loop
                          // (Tron, voxel) / the printer's keep-warm
        F32 aux0[4];      // printer: its build's box centre (draw frame), w = 1; shatter: the impact direction
        F32 aux1[4];      // printer: that box's half extents, w = infill rows per layer
    };
    static_assert(sizeof(Slot) == 80, "Slot must match gmdx_rezfx.glsl RezFxSlot (std430)");

    // Once per frame before the store is mirrored (LLCleanRendererVK::syncFromStoresPrestep):
    // starts rez-ins for objects whose first mesh registered, moves stages on, ends effects
    // and releases dying objects whose de-rez is over. Returns at once when nothing runs.
    void update();
    // LLViewerObject::markDead, before the store would release the object's rows. True: a
    // de-rez took the rows over (they are released when it ends); false: release them now.
    bool objectKilled(LLViewerObject* obj);
    // What the effects draw beside the objects themselves (gmdxrezfx.cpp): the transporter's beam
    // column, the 3D printer's nozzle and gantry, and the de-rez debris (ash flakes and smoke,
    // disintegrate trails, shatter glints). gGL drawing in the world-overlay segment
    // (llappviewer.cpp), depth test on, no depth write. Returns at once while no effect runs.
    void renderOverlay();
    // A build effect (3D printer / transporter) in hidden mode is running: its opaque rigged rows
    // sit in the rigged mask buckets (clean_cull.comp), which the shadow views then cast even at
    // avatar shadow detail 0 (llcleanrenderervk.cpp recordShadows / recordProjShadows).
    bool hiding();
    // The table for upload (rows [0, count)); gen changes whenever its content does.
    const Slot* table(U32& count, U32& gen);
    // Fragment-shader barycentrics (the Tron wireframe lines): set by the renderer at init.
    void setWireframe(bool on);
    bool wireframe();
    // The renderer's lit FS modules (by embedded symbol name, "kWorldFragSpv" or "Frag"): on a
    // device with barycentrics, swaps code / bytes for the module's Tron-wireframe twin
    // (vkgfx/gmdxrezfxvk.cpp, which alone holds those twins). Any thread.
    void wireModule(const char* name, const uint32_t*& code, size_t& bytes);
}
