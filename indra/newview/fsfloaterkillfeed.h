/**
 * @file fsfloaterkillfeed.h
 * @brief Kill feed: Combat 2.0 DEATH events shown as a text-only screen
 *        overlay. This floater is the settings panel (enable, text size,
 *        position, lines, hold time); the overlay itself renders from
 *        LLViewerWindow::draw.
 *
 *        GunMetal-DX: the events come from the #GunMetal-DX Helper bridge
 *        (slbridge, "#slb:combat" lines), which is worn only while the kill
 *        feed is enabled.
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

#ifndef FS_FLOATERKILLFEED_H
#define FS_FLOATERKILLFEED_H

#include "llfloater.h"

#include <deque>

class FSFloaterKillFeed : public LLFloater
{
public:
    FSFloaterKillFeed(const LLSD& key);
    virtual ~FSFloaterKillFeed() = default;

    // After login: hooks the bridge and follows FSKillFeedEnabled, wearing the
    // helper (and listening to the combat log) only while the feed is on.
    static void init();

    // A "#slb:combat|<victim>|<killer>|<type>|<damage>|<weapon>" line from the
    // bridge (the part after "combat|").
    static void handleBridgeEvent(const std::string& line);

    // Text-only kill feed overlay, rendered every frame from
    // LLViewerWindow::draw when FSKillFeedEnabled is set. Position, scale,
    // line count and hold time come from saved settings.
    static void drawOverlay();

    // Drop every held kill. Called from reset_login() so a never-fade feed
    // (Hold = 0) does not carry the last session's kills onto the next login.
    static void clearEntries();

    struct KillEntry
    {
        F64         mReceivedTime;  // LLFrameTimer::getTotalSeconds() at receipt
        LLUUID      mVictim;        // DEATH "target"
        LLUUID      mKiller;        // DEATH "owner" (falls back to "source")
        std::string mWeapon;        // bridge-resolved weapon name, may be empty
        S32         mType;          // DEATH "type", -1000 unknown
        F32         mDamage;        // killing blow damage (its DAMAGE in the same combat log message), <0 unknown
        F32         mDistance;      // meters between killer and victim avatars, <0 unknown
    };

private:
    typedef std::deque<KillEntry> entries_t;

    static entries_t sEntries;

    static void updateBridgeUse();
};

#endif // FS_FLOATERKILLFEED_H
