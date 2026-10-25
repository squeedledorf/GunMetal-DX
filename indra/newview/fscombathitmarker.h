/**
 * @file fscombathitmarker.h
 * @brief Hitmarker and hit report for outgoing Combat 2.0 damage: a marker
 *        flash at the crosshair (white on hit, red on kill), an optional
 *        fading hit report line and hit/kill sounds. Events arrive
 *        owner-filtered from the LSL bridge combat log listener.
 *
 *        GunMetal-DX: the bridge is the #GunMetal-DX Helper (slbridge, "#slb:hit"
 *        lines), worn only while the hit marker is enabled. The custom crosshair,
 *        its tint and the OTS convergence parts of SoapStorm's file live in
 *        fscombataim.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 *
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

#ifndef FS_COMBATHITMARKER_H
#define FS_COMBATHITMARKER_H

#include <string>

class FSCombatHitMarker
{
public:
    // After login: hooks the bridge and follows FSHitMarkerEnabled, wearing the
    // helper (and asking it for the owner's hits) only while the hit marker is on.
    static void init();

    // A "#slb:hit|<D or K>|<target>|<agent>|<type>|<damage>" line from the bridge
    // (the part after "hit|"): one of the agent's own hits (D) or kills (K).
    static void handleBridgeEvent(const std::string& line);

    // Per-frame overlay: hitmarker flash and fading hit report text.
    // Renders in any camera mode. Called from LLViewerWindow::draw.
    static void draw();

    // Play the hit/kill sound at the current FSHitMarkerSoundGain, bypassing the
    // master "Play sounds" toggle, so the Test buttons can preview levels even
    // with sounds otherwise off.
    static void playHitSoundPreview();
    static void playKillSoundPreview();

    // --- Damage type symbol table (for the symbols editor) ---------------

    struct DamageTypeInfo
    {
        S32         mType;          // combat log damage type number
        const char* mName;          // display name
        const char* mDefaultEmoji;  // built-in symbol
    };
    static S32 getDamageTypeCount();
    static const DamageTypeInfo& getDamageTypeInfo(S32 index);

    // Effective symbol for a type (override if present, else default).
    static std::string getEmojiForType(S32 type);
    // Set (or clear with an empty string) the user override for a type;
    // persists into the FSHitMarkerEmojiMap setting.
    static void setEmojiOverride(S32 type, const std::string& symbol);
    static void clearEmojiOverrides();
};

#endif // FS_COMBATHITMARKER_H
