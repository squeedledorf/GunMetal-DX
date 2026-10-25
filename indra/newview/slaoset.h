/**
 * @file slaoset.h
 * @brief Client-side animation overrider (#496): animation states, the
 *        in-memory model of one AO set, and the AO notecard parser.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
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

#ifndef SL_AOSET_H
#define SL_AOSET_H

#include <string>
#include <vector>

#include "llsd.h"
#include "lluuid.h"

namespace SLAO
{
    // Animation states the AO can replace. The order is the on-disk and UI
    // order; append new states at the end of a group, never renumber
    // persisted data (states are stored by folder name, not by number).
    enum EState
    {
        STATE_NONE = -1,
        STAND = 0,
        WALK,
        RUN,
        CROUCH,
        CROUCH_WALK,
        TURN_LEFT,
        TURN_RIGHT,
        PRE_JUMP,
        JUMP,
        FALL,
        LAND_SOFT,           // soft landing ("land")
        LAND_MEDIUM,    // medium landing; falls back to LAND
        STAND_UP,       // hard landing ("standup" after a long fall)
        FLY,
        FLY_SLOW,       // falls back to FLY
        HOVER,
        HOVER_UP,
        HOVER_DOWN,
        SIT,            // sitting on an object
        SIT_GROUND,
        TYPING,         // overlay: plays on top of the posture state
        FLOATING,          // client-side water states (hovering / flying under water)
        SWIM_FORWARD,
        SWIM_UP,
        SWIM_DOWN,
        WALK_BACK,      // walking backwards (walk built-in + back key); falls back to WALK
        ALWAYS,         // body-fix animations played all the time while the AO is on
        IDLE,           // random idles played now and then in place of the stand
        STRIDE,         // the built-in "stride" (walking into an obstacle); falls back to WALK
        TAKE_OFF,       // hovering up right after leaving the ground; falls back to HOVER_UP
        AWAY,           // standing while away (AFK); falls back to STAND
        BUSY,           // standing while busy (do not disturb); falls back to STAND
        FLY_LEFT,       // flying while turning (turn keys); falls back to FLY
        FLY_RIGHT,
        RUN_LEFT,       // running while turning; falls back to RUN
        RUN_RIGHT,
        SWIM_FAST,      // flying fast under water; falls back to SWIM_FORWARD
        SWIM_SURFACE,   // hovering at the water surface; falls back to FLOATING
        SEA_WALK,       // walking on the sea floor; falls back to WALK
        SEA_RUN,        // running on the sea floor; falls back to RUN
        STATE_COUNT
    };

    // Canonical inventory folder name of a state. These are the section
    // names of ZHAO-II notecards without the brackets, which is also the
    // folder naming used by other viewers' client AOs.
    const std::string& stateFolderName(EState state);
    // Short label for the UI.
    std::string stateLabel(EState state);
    // Tolerant lookup of a state folder name (case-insensitive, ignores a
    // ":"-separated suffix and common aliases). STATE_NONE if unknown.
    EState stateFromFolderName(const std::string& name);
    // Section token from an AO notecard: "[ Standing ]", "[Stand.1]",
    // "[Sit.G]", ... STATE_NONE if unknown.
    EState stateFromNotecardToken(const std::string& token);

    // Best guess of the state from an animation's name ("OP Stand 3",
    // "walk_f", "HoverUp"), STATE_NONE when it is not obvious.
    EState guessStateFromAnimName(const std::string& name);

    // Built-in (Linden) state animation -> AO state, STATE_NONE otherwise.
    EState stateForDefaultAnim(const LLUUID& anim_id);
    // True for every built-in animation, state or not (emotes, typing, ...).
    bool isBuiltinAnim(const LLUUID& anim_id);
    // Ranking used when the server signals several state animations at
    // once; the highest wins the posture channel.
    S32 stateRank(EState state);

    // State whose animations are used when this state has none.
    EState fallbackState(EState state);
    // Posture states a script animation (pose, dance, poseball) pauses.
    bool isIdleState(EState state);
    bool isSitState(EState state);
    // Whether the AO also asks the server to stop the built-in animation
    // while its replacement plays (see the table in slaoset.cpp).
    bool stopsDefaultAnim(EState state);
}

struct SLAOAnim
{
    LLUUID      mItemID;    // inventory item in the state folder (usually a link)
    LLUUID      mTargetID;  // the real animation item a link points at (or mItemID)
    LLUUID      mAssetID;
    std::string mName;
};

struct SLAOStateConfig
{
    bool mCycle = false;        // switch animation every mInterval seconds
    bool mRandom = false;       // random order instead of list order
    // Pick (next, or random) only when the state starts and loop that one
    // while it lasts; no timed switching
    bool mPickOnEntry = false;
    F32  mInterval = 30.f;      // seconds

    LLSD toLLSD() const;
    void fromLLSD(const LLSD& sd);
};

class SLAOSet
{
public:
    SLAOSet();

    bool hasAnims(SLAO::EState state) const;
    // State whose list is used for `state` (follows fallbacks), or
    // STATE_NONE when neither the state nor its fallbacks have animations.
    SLAO::EState resolveState(SLAO::EState state) const;
    S32 countAnims() const;

    LLUUID      mFolderID;
    std::string mName;          // display name
    bool        mFromOtherViewer = false;   // lives under another viewer's #AO root
    bool        mFromSnapshot = false;      // restored from disk, inventory not loaded yet

    LLUUID                      mStateFolders[SLAO::STATE_COUNT];
    std::vector<SLAOAnim>       mAnims[SLAO::STATE_COUNT];
    SLAOStateConfig             mStateConfig[SLAO::STATE_COUNT];

    // Copies taken from an object's contents (see SLAOSets::COPIES_FOLDER_NAME)
    LLUUID                      mCopiesFolderID;
    bool                        mHasCopies = false;
    std::vector<SLAOAnim>       mUnsorted;      // copies no state uses yet

    // Per-set options
    bool mSitOverride = true;           // replace sits on objects
    bool mSmartSit = true;              // ... unless the seat animates us
    bool mNoStandsInMouselook = false;
    bool mNoStandsWhileTyping = false;
    bool mStopSystemAnims = true;       // stop the built-ins under our replacements
    bool mAlwaysEnabled = true;         // play the Always list
    bool mIdleEnabled = true;           // play the Idle list now and then
    F32  mIdleMinDelay = 30.f;          // seconds between idles, randomised in [min, max]
    F32  mIdleMaxDelay = 90.f;
    F32  mIdleMaxDuration = 15.f;       // cap for looped (or long) idles
    bool mIdleWhileSitting = false;     // idles may replace the sit override too
};

// Result of parsing a ZHAO-II / Oracul style AO notecard.
struct SLAONotecard
{
    std::vector<std::string> mNames[SLAO::STATE_COUNT]; // animation names, notecard order
    std::vector<std::string> mIgnoredSections;          // unknown [section] tokens
    std::string mFormat;                                // "ZHAO-II", "Oracul", "mixed"
    S32 mStateCount = 0;
    S32 mAnimCount = 0;

    // Parses notecard text (already stripped of the "Linden text" header).
    // Returns true if at least one state section with an animation was found.
    bool parse(const std::string& text);

    // Runs a real AO product notecard (with its formatting quirks) through
    // the parser and checks every section lands where it should. Used by
    // "/ao selftest"; `report` says what failed.
    static bool selfTest(std::string& report);
};

#endif // SL_AOSET_H
