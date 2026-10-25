/**
 * @file slaoset.cpp
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

#include "llviewerprecompiledheaders.h"

#include "slaoset.h"

#include <algorithm>
#include <map>
#include <set>
#include <sstream>

#include "llanimationstates.h"
#include "llstring.h"
#include "lltrans.h"

using namespace SLAO;

namespace
{
    struct StateInfo
    {
        const char* mFolder;    // canonical folder / ZHAO-II section name
        const char* mLabelKey;  // strings.xml key for the UI label
        S32         mRank;      // posture channel priority
        EState      mFallback;
        bool        mIdle;      // paused while a script animates the body
        // Ask the server to stop the built-in animation as well (when the set's
        // "Stop system animations" is on). True for every posture state; the
        // exceptions and why:
        //  - ground sit: when its built-in leaves the list the viewer stands
        //    the avatar up (LLVOAvatar::sitDown(false));
        //  - typing: handled by the overlay channel, not the posture channel;
        //  - Always / Idle: not posture states.
        // (The do-not-disturb animation is never stopped: the viewer
        // re-asserts it; see LLVOAvatar::processSingleAnimationStateChange.)
        //
        // Why: a replacement only hides the built-in on the joints it animates
        // at a higher priority; elsewhere, and on priority ties, the built-in
        // leaks through (hover arms, turn feet, walk arm swing, ...). The
        // built-in comes back as soon as the AO lets go of the state.
        bool        mStopDefault;
    };

    const StateInfo STATE_INFO[STATE_COUNT] =
    {
        // folder               label key                   rank fallback     idle   stop
        { "Standing",           "SLAOState_Standing",         10, STATE_NONE, true,  true  }, // STAND
        { "Walking",            "SLAOState_Walking",          49, STATE_NONE, false, true  }, // WALK
        { "Running",            "SLAOState_Running",          50, STATE_NONE, false, true  }, // RUN
        { "Crouching",          "SLAOState_Crouching",        59, STATE_NONE, true,  true  }, // CROUCH
        { "Crouch Walking",     "SLAOState_CrouchWalking",    60, STATE_NONE, false, true  }, // CROUCH_WALK
        // Turning in place without turn animations keeps the stand playing
        { "Turning Left",       "SLAOState_TurningLeft",      40, STAND,      true,  true  }, // TURN_LEFT
        { "Turning Right",      "SLAOState_TurningRight",     40, STAND,      true,  true  }, // TURN_RIGHT
        { "Pre Jumping",        "SLAOState_PreJumping",       85, STATE_NONE, false, true  }, // PRE_JUMP
        { "Jumping",            "SLAOState_Jumping",          84, STATE_NONE, false, true  }, // JUMP
        { "Falling",            "SLAOState_Falling",          83, STATE_NONE, false, true  }, // FALL
        { "Landing",            "SLAOState_Landing",          87, STATE_NONE, false, true  }, // LAND_SOFT
        { "Medium Landing",     "SLAOState_MediumLanding",    88, LAND_SOFT,       false, true  }, // LAND_MEDIUM
        { "Standing Up",        "SLAOState_StandingUp",       90, STATE_NONE, false, true  }, // STAND_UP
        { "Flying",             "SLAOState_Flying",           78, STATE_NONE, false, true  }, // FLY
        { "Flying Slow",        "SLAOState_FlyingSlow",       77, FLY,        false, true  }, // FLY_SLOW
        { "Hovering",           "SLAOState_Hovering",         76, STATE_NONE, true,  true  }, // HOVER
        { "Flying Up",          "SLAOState_FlyingUp",         80, HOVER,      false, true  }, // HOVER_UP
        { "Flying Down",        "SLAOState_FlyingDown",       79, HOVER,      false, true  }, // HOVER_DOWN
        { "Sitting",            "SLAOState_Sitting",          95, STATE_NONE, true,  true  }, // SIT
        // Never stop the built-in ground sit: when it leaves the animation
        // list the viewer treats the avatar as standing up (LLVOAvatar::sitDown).
        { "Sitting On Ground",  "SLAOState_SittingOnGround", 100, STATE_NONE, true,  false }, // SIT_GROUND
        // Typing is an overlay: the built-in typing animation is requested by
        // the viewer itself while typing, so it is never stopped.
        { "Typing",             "SLAOState_Typing",            0, STATE_NONE, true,  false }, // TYPING
        // Water states are derived client-side from the hover/fly states and
        // fall back to them, so they never stop anything on their own.
        { "Floating",           "SLAOState_Floating",         76, HOVER,      true,  true  }, // FLOATING
        { "Swimming Forward",   "SLAOState_SwimmingForward",  78, FLY,        false, true  }, // SWIM_FORWARD
        { "Swimming Up",        "SLAOState_SwimmingUp",       80, HOVER_UP,   false, true  }, // SWIM_UP
        { "Swimming Down",      "SLAOState_SwimmingDown",     79, HOVER_DOWN, false, true  }, // SWIM_DOWN
        // Derived client-side from the walk built-in and the back key
        { "Walking Backwards",  "SLAOState_WalkingBackwards", 49, WALK,       false, true  }, // WALK_BACK
        // Not posture states: Always plays beside everything, Idle replaces
        // the stand now and then. Neither stops a built-in.
        { "Always",             "SLAOState_Always",            0, STATE_NONE, false, false }, // ALWAYS
        { "Idle",               "SLAOState_Idle",              0, STATE_NONE, true,  false }, // IDLE
        // Server-signalled ("stride" built-in): ranked above walk so it wins when both play
        { "Striding",           "SLAOState_Striding",         51, WALK,       false, true  }, // STRIDE
        // The rest are derived client-side from a base state (see SLAOEngine::effectiveState)
        { "Taking Off",         "SLAOState_TakingOff",        81, HOVER_UP,   false, true  }, // TAKE_OFF
        { "Away",               "SLAOState_Away",             10, STAND,      true,  true  }, // AWAY
        { "Busy",               "SLAOState_Busy",             10, STAND,      true,  true  }, // BUSY
        { "Flying Left",        "SLAOState_FlyingLeft",       78, FLY,        false, true  }, // FLY_LEFT
        { "Flying Right",       "SLAOState_FlyingRight",      78, FLY,        false, true  }, // FLY_RIGHT
        { "Running Left",       "SLAOState_RunningLeft",      50, RUN,        false, true  }, // RUN_LEFT
        { "Running Right",      "SLAOState_RunningRight",     50, RUN,        false, true  }, // RUN_RIGHT
        { "Swimming Fast",      "SLAOState_SwimmingFast",     78, SWIM_FORWARD, false, true }, // SWIM_FAST
        { "Swimming Surface",   "SLAOState_SwimmingSurface",  76, FLOATING,      true,  true  }, // SWIM_SURFACE
        { "Sea Walk",           "SLAOState_SeaWalk",          49, WALK,       false, true  }, // SEA_WALK
        { "Sea Run",            "SLAOState_SeaRun",           50, RUN,        false, true  }, // SEA_RUN
    };

    bool valid(EState state)
    {
        return state > STATE_NONE && state < STATE_COUNT;
    }

    // Lower-case, keep only letters and digits.
    std::string squash(const std::string& in)
    {
        std::string out;
        out.reserve(in.size());
        for (char c : in)
        {
            if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            {
                out.push_back(c);
            }
            else if (c >= 'A' && c <= 'Z')
            {
                out.push_back(c - 'A' + 'a');
            }
        }
        return out;
    }

    // Squashed aliases from ZHAO-II section names, Oracul slot names
    // ("Stand.1", "Sit.G", "Hover.U", ...) and llSetAnimationOverride state
    // names. Trailing slot numbers are stripped before lookup.
    const std::map<std::string, EState>& alias_map()
    {
        static std::map<std::string, EState> sMap;
        if (sMap.empty())
        {
            const std::pair<const char*, EState> aliases[] =
            {
                { "standing", STAND }, { "stand", STAND }, { "standn", STAND }, { "stands", STAND },
                { "walking", WALK }, { "walk", WALK }, { "walkn", WALK }, { "walks", WALK },
                { "running", RUN }, { "run", RUN }, { "runn", RUN },
                { "crouching", CROUCH }, { "crouch", CROUCH }, { "crouchn", CROUCH },
                { "crouchwalking", CROUCH_WALK }, { "crouchwalk", CROUCH_WALK }, { "crouchw", CROUCH_WALK },
                { "turningleft", TURN_LEFT }, { "turnleft", TURN_LEFT }, { "turnl", TURN_LEFT },
                { "turningright", TURN_RIGHT }, { "turnright", TURN_RIGHT }, { "turnr", TURN_RIGHT },
                { "prejumping", PRE_JUMP }, { "prejump", PRE_JUMP }, { "jumpp", PRE_JUMP },
                { "jumping", JUMP }, { "jump", JUMP }, { "jumpn", JUMP },
                { "falling", FALL }, { "fall", FALL }, { "falln", FALL }, { "fallingdown", FALL },
                { "landing", LAND_SOFT }, { "land", LAND_SOFT }, { "landn", LAND_SOFT }, { "softlanding", LAND_SOFT }, { "softland", LAND_SOFT },
                { "mediumlanding", LAND_MEDIUM }, { "mediumland", LAND_MEDIUM }, { "landm", LAND_MEDIUM },
                { "standingup", STAND_UP }, { "standup", STAND_UP }, { "standu", STAND_UP }, { "hardlanding", STAND_UP },
                { "flying", FLY }, { "fly", FLY }, { "flyn", FLY },
                { "flyingslow", FLY_SLOW }, { "flyslow", FLY_SLOW }, { "flys", FLY_SLOW },
                { "hovering", HOVER }, { "hover", HOVER }, { "hovern", HOVER },
                { "flyingup", HOVER_UP }, { "hoveringup", HOVER_UP }, { "hoverup", HOVER_UP }, { "hoveru", HOVER_UP },
                { "flyingdown", HOVER_DOWN }, { "hoveringdown", HOVER_DOWN }, { "hoverdown", HOVER_DOWN }, { "hoverd", HOVER_DOWN },
                { "sitting", SIT }, { "sit", SIT }, { "sitn", SIT }, { "sits", SIT },
                { "sittingonground", SIT_GROUND }, { "sitonground", SIT_GROUND }, { "sittingground", SIT_GROUND },
                { "groundsit", SIT_GROUND }, { "groundsitting", SIT_GROUND }, { "sitground", SIT_GROUND }, { "sitg", SIT_GROUND },
                { "typing", TYPING }, { "type", TYPING }, { "typen", TYPING },
                { "floating", FLOATING }, { "float", FLOATING }, { "swimh", FLOATING }, { "swimhover", FLOATING },
                { "swimmingforward", SWIM_FORWARD }, { "swimforward", SWIM_FORWARD }, { "swimming", SWIM_FORWARD },
                { "swim", SWIM_FORWARD }, { "swimn", SWIM_FORWARD }, { "swimf", SWIM_FORWARD },
                { "swimmingup", SWIM_UP }, { "swimup", SWIM_UP }, { "swimu", SWIM_UP },
                { "swimmingdown", SWIM_DOWN }, { "swimdown", SWIM_DOWN }, { "swimd", SWIM_DOWN },
                { "walkingbackwards", WALK_BACK }, { "walkingbackward", WALK_BACK }, { "walkbackwards", WALK_BACK },
                { "walkbackward", WALK_BACK }, { "walkingback", WALK_BACK }, { "walkback", WALK_BACK },
                { "backwards", WALK_BACK }, { "backward", WALK_BACK }, { "backwalk", WALK_BACK },
                { "walkb", WALK_BACK }, { "walkbk", WALK_BACK },
                { "always", ALWAYS }, { "alwayson", ALWAYS }, { "alwaysplay", ALWAYS }, { "bodyfix", ALWAYS },
                { "bodyfixes", ALWAYS }, { "fixes", ALWAYS },
                { "deformer", ALWAYS }, { "deformers", ALWAYS },
                { "striding", STRIDE }, { "stride", STRIDE },
                { "takingoff", TAKE_OFF }, { "takeoff", TAKE_OFF },
                { "away", AWAY }, { "afk", AWAY },
                { "busy", BUSY }, { "dnd", BUSY }, { "donotdisturb", BUSY },
                { "flyingleft", FLY_LEFT }, { "flyleft", FLY_LEFT }, { "flyl", FLY_LEFT },
                { "flyingright", FLY_RIGHT }, { "flyright", FLY_RIGHT }, { "flyr", FLY_RIGHT },
                { "runningleft", RUN_LEFT }, { "runleft", RUN_LEFT }, { "runl", RUN_LEFT },
                { "runningright", RUN_RIGHT }, { "runright", RUN_RIGHT }, { "runr", RUN_RIGHT },
                { "swimmingfast", SWIM_FAST }, { "swimfast", SWIM_FAST },
                { "swimmingidle", FLOATING }, { "swimidle", FLOATING },
                { "swimmingsurface", SWIM_SURFACE }, { "swimsurface", SWIM_SURFACE }, { "surface", SWIM_SURFACE },
                { "seawalk", SEA_WALK }, { "seawalking", SEA_WALK }, { "searun", SEA_RUN }, { "searunning", SEA_RUN },
                { "idle", IDLE }, { "idles", IDLE }, { "idling", IDLE }, { "randomidle", IDLE },
                { "randomidles", IDLE }, { "fidget", IDLE }, { "fidgets", IDLE },
            };
            for (const auto& [name, state] : aliases)
            {
                sMap[name] = state;
            }
        }
        return sMap;
    }

    EState lookup_alias(const std::string& raw)
    {
        std::string key = squash(raw);
        const auto& aliases = alias_map();
        auto it = aliases.find(key);
        if (it != aliases.end())
        {
            return it->second;
        }
        // "Stand.1", "Stand2", "Walk.N" -> drop trailing slot digits
        while (!key.empty() && key.back() >= '0' && key.back() <= '9')
        {
            key.pop_back();
        }
        it = aliases.find(key);
        return (it != aliases.end()) ? it->second : STATE_NONE;
    }

    const std::map<LLUUID, EState>& default_anim_map()
    {
        static std::map<LLUUID, EState> sMap;
        if (sMap.empty())
        {
            const std::pair<const LLUUID*, EState> defaults[] =
            {
                { &ANIM_AGENT_STAND, STAND }, { &ANIM_AGENT_STAND_1, STAND }, { &ANIM_AGENT_STAND_2, STAND },
                { &ANIM_AGENT_STAND_3, STAND }, { &ANIM_AGENT_STAND_4, STAND },
                { &ANIM_AGENT_WALK, WALK }, { &ANIM_AGENT_FEMALE_WALK, WALK },
                { &ANIM_AGENT_WALK_NEW, WALK }, { &ANIM_AGENT_FEMALE_WALK_NEW, WALK },
                { &ANIM_AGENT_RUN, RUN }, { &ANIM_AGENT_RUN_NEW, RUN }, { &ANIM_AGENT_FEMALE_RUN_NEW, RUN },
                { &ANIM_AGENT_CROUCH, CROUCH }, { &ANIM_AGENT_CROUCHWALK, CROUCH_WALK },
                { &ANIM_AGENT_TURNLEFT, TURN_LEFT }, { &ANIM_AGENT_TURNRIGHT, TURN_RIGHT },
                { &ANIM_AGENT_PRE_JUMP, PRE_JUMP }, { &ANIM_AGENT_JUMP, JUMP },
                { &ANIM_AGENT_FALLDOWN, FALL },
                { &ANIM_AGENT_LAND, LAND_SOFT }, { &ANIM_AGENT_MEDIUM_LAND, LAND_MEDIUM }, { &ANIM_AGENT_STANDUP, STAND_UP },
                { &ANIM_AGENT_FLY, FLY }, { &ANIM_AGENT_FLYSLOW, FLY_SLOW },
                { &ANIM_AGENT_HOVER, HOVER }, { &ANIM_AGENT_HOVER_UP, HOVER_UP }, { &ANIM_AGENT_HOVER_DOWN, HOVER_DOWN },
                { &ANIM_AGENT_SIT, SIT }, { &ANIM_AGENT_SIT_GENERIC, SIT }, { &ANIM_AGENT_SIT_FEMALE, SIT },
                { &ANIM_AGENT_SIT_GROUND, SIT_GROUND }, { &ANIM_AGENT_SIT_GROUND_CONSTRAINED, SIT_GROUND },
                { &ANIM_AGENT_TYPE, TYPING },
                { &ANIM_AGENT_STRIDE, STRIDE },
            };
            for (const auto& [id, state] : defaults)
            {
                sMap[*id] = state;
            }
        }
        return sMap;
    }
}

namespace SLAO
{
    const std::string& stateFolderName(EState state)
    {
        static std::vector<std::string> sNames;
        if (sNames.empty())
        {
            for (const StateInfo& info : STATE_INFO)
            {
                sNames.emplace_back(info.mFolder);
            }
        }
        return valid(state) ? sNames[state] : LLStringUtil::null;
    }

    std::string stateLabel(EState state)
    {
        if (!valid(state))
        {
            return std::string();
        }
        std::string label;
        if (LLTrans::findString(label, STATE_INFO[state].mLabelKey))
        {
            return label;
        }
        return STATE_INFO[state].mFolder;
    }

    EState stateFromFolderName(const std::string& name)
    {
        // Other viewers may append ":"-separated option flags to folder names
        std::string base = name;
        const std::string::size_type colon = base.find(':');
        if (colon != std::string::npos)
        {
            base.erase(colon);
        }
        LLStringUtil::trim(base);
        for (S32 i = 0; i < STATE_COUNT; ++i)
        {
            if (LLStringUtil::compareInsensitive(base, STATE_INFO[i].mFolder) == 0)
            {
                return (EState)i;
            }
        }
        return lookup_alias(base);
    }

    EState stateFromNotecardToken(const std::string& token)
    {
        return lookup_alias(token);
    }

    EState guessStateFromAnimName(const std::string& name)
    {
        // Words of the name, lower case, trailing digits dropped
        std::set<std::string> words;
        std::string word;
        auto flush = [&words, &word]()
        {
            while (!word.empty() && word.back() >= '0' && word.back() <= '9')
            {
                word.pop_back();
            }
            if (!word.empty())
            {
                words.insert(word);
            }
            word.clear();
        };
        for (char c : name)
        {
            if (c >= 'A' && c <= 'Z')
            {
                // camelCase splits: "HoverUp" -> "hover", "up"
                if (!word.empty() && word.back() >= 'a' && word.back() <= 'z')
                {
                    flush();
                }
                word.push_back((char)(c - 'A' + 'a'));
            }
            else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
            {
                word.push_back(c);
            }
            else
            {
                flush();
            }
        }
        flush();
        auto has = [&words](const char* w) { return words.count(w) != 0; };

        const bool up = has("up");
        const bool down = has("down") || has("dn");
        const bool left = has("left") || has("l");
        const bool right = has("right") || has("r");
        if (has("typing") || has("type"))
            return TYPING;
        if (has("groundsit") || has("gsit") || has("sitground") || (has("sit") && has("ground")))
            return SIT_GROUND;
        if (has("sit") || has("sitting") || has("sits"))
            return SIT;
        if (has("swim") || has("swimming") || has("swimup") || has("swimdown") || has("swimfast") ||
            has("swimidle") || has("swimsurface") || has("swimforward") || has("swimfoward"))
        {
            if (has("surface") || has("swimsurface"))
                return SWIM_SURFACE;
            if (has("idle") || has("swimidle"))
                return FLOATING;
            if (has("fast") || has("swimfast"))
                return SWIM_FAST;
            if (up || has("swimup"))
                return SWIM_UP;
            if (down || has("swimdown"))
                return SWIM_DOWN;
            return SWIM_FORWARD;
        }
        if (has("float") || has("floating") || has("tread"))
            return FLOATING;
        if (has("hover") || has("hovering"))
            return up ? HOVER_UP : (down ? HOVER_DOWN : HOVER);
        if (has("fly") || has("flying") || has("flyp"))
        {
            if (up)
                return HOVER_UP;
            if (down)
                return HOVER_DOWN;
            if (has("turnl") || (has("turn") && left))
                return FLY_LEFT;
            if (has("turnr") || (has("turn") && right))
                return FLY_RIGHT;
            return has("slow") ? FLY_SLOW : FLY;
        }
        if (has("prejump") || (has("pre") && has("jump")))
            return PRE_JUMP;
        if (has("jump") || has("jumping"))
            return JUMP;
        if (has("fall") || has("falling"))
            return FALL;
        if (has("standup") || (has("stand") && up))
            return STAND_UP;
        if (has("land") || has("landing"))
            return LAND_SOFT;
        if (has("turn") || has("turning"))
            return left ? TURN_LEFT : (right ? TURN_RIGHT : STATE_NONE);
        if (has("crouchwalk") || (has("crouch") && (has("walk") || has("walking"))))
            return CROUCH_WALK;
        if (has("crouch") || has("crouching"))
            return CROUCH;
        if (has("always") || has("bodyfix") || has("fix") || has("deformer"))
            return ALWAYS;
        if (has("sea") && (has("run") || has("running")))
            return SEA_RUN;
        if (has("sea") && (has("walk") || has("walking")))
            return SEA_WALK;
        if (has("takeoff") || (has("take") && has("off")) || has("takingoff"))
            return TAKE_OFF;
        if (has("away") || has("afk") || has("sleepyaway"))
            return AWAY;
        if (has("busy") || has("dnd"))
            return BUSY;
        if (has("stride") || has("striding"))
            return STRIDE;
        if ((has("walk") || has("walking") || has("walkback")) &&
            (has("back") || has("backwards") || has("backward") || has("walkback")))
            return WALK_BACK;
        if (has("backwards") || has("backward"))
            return WALK_BACK;
        if ((has("idle") || has("idles") || has("fidget")) && !has("stand"))
            return IDLE;
        if (has("run") || has("running") || has("runp"))
        {
            if (has("turnl") || (has("turn") && left))
                return RUN_LEFT;
            if (has("turnr") || (has("turn") && right))
                return RUN_RIGHT;
            return RUN;
        }
        if (has("walk") || has("walking"))
            return WALK;
        if (has("stand") || has("standing"))
            return STAND;
        return STATE_NONE;
    }

    EState stateForDefaultAnim(const LLUUID& anim_id)
    {
        const auto& defaults = default_anim_map();
        auto it = defaults.find(anim_id);
        return (it != defaults.end()) ? it->second : STATE_NONE;
    }

    bool isBuiltinAnim(const LLUUID& anim_id)
    {
        return gAnimLibrary.animStateToString(anim_id) != nullptr;
    }

    S32 stateRank(EState state)
    {
        return valid(state) ? STATE_INFO[state].mRank : -1;
    }

    EState fallbackState(EState state)
    {
        return valid(state) ? STATE_INFO[state].mFallback : STATE_NONE;
    }

    bool isIdleState(EState state)
    {
        return valid(state) && STATE_INFO[state].mIdle;
    }

    bool isSitState(EState state)
    {
        return state == SIT || state == SIT_GROUND;
    }

    bool stopsDefaultAnim(EState state)
    {
        return valid(state) && STATE_INFO[state].mStopDefault;
    }
}

//-----------------------------------------------------------------------------
// SLAOStateConfig
//-----------------------------------------------------------------------------

LLSD SLAOStateConfig::toLLSD() const
{
    LLSD sd = LLSD::emptyMap();
    sd["cycle"] = mCycle;
    sd["random"] = mRandom;
    sd["pick_on_entry"] = mPickOnEntry;
    sd["interval"] = mInterval;
    return sd;
}

void SLAOStateConfig::fromLLSD(const LLSD& sd)
{
    if (!sd.isMap())
    {
        return;
    }
    if (sd.has("cycle"))
    {
        mCycle = sd["cycle"].asBoolean();
    }
    if (sd.has("random"))
    {
        mRandom = sd["random"].asBoolean();
    }
    if (sd.has("pick_on_entry"))
    {
        mPickOnEntry = sd["pick_on_entry"].asBoolean();
    }
    // Files from the build that defaulted "change on entry" on for every
    // state stored it without cycling; never let that switch cycling on
    if (!mCycle)
    {
        mPickOnEntry = false;
    }
    if (sd.has("interval"))
    {
        mInterval = llclamp((F32)sd["interval"].asReal(), 1.f, 3600.f);
    }
}

//-----------------------------------------------------------------------------
// SLAOSet
//-----------------------------------------------------------------------------

SLAOSet::SLAOSet()
{
    // Only stands cycle by default (on a timer), as every AO product does.
    // Everything else plays the first animation of its list until the user
    // turns cycling on for it.
    mStateConfig[STAND].mCycle = true;
    // Idles pick a random one each time by default
    mStateConfig[IDLE].mCycle = true;
    mStateConfig[IDLE].mRandom = true;
}

bool SLAOSet::hasAnims(EState state) const
{
    return valid(state) && !mAnims[state].empty();
}

EState SLAOSet::resolveState(EState state) const
{
    // Bounded walk: fallback chains are at most two long
    for (S32 hops = 0; hops < 4 && valid(state); ++hops)
    {
        if (hasAnims(state))
        {
            return state;
        }
        state = fallbackState(state);
    }
    return STATE_NONE;
}

S32 SLAOSet::countAnims() const
{
    S32 count = 0;
    for (const auto& anims : mAnims)
    {
        count += (S32)anims.size();
    }
    return count;
}

//-----------------------------------------------------------------------------
// SLAONotecard
//
// ZHAO-II:  [ Standing ]anim one|anim two|anim three
//           Repeated sections append. Lines starting with '#' are comments.
// Oracul:   [Stand.1]anim one,anim two     (slots Stand.1/2/3, Sit.G, Hover.U, ...)
// Both separators are accepted on any line; a line without '|' is split on
// ',' only (Oracul), so ZHAO names containing commas survive.
//-----------------------------------------------------------------------------

bool SLAONotecard::parse(const std::string& text)
{
    bool saw_zhao = false;
    bool saw_oracul = false;

    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
    {
        LLStringUtil::trim(line);
        if (line.empty() || line[0] == '#' || line[0] != '[')
        {
            continue;
        }
        const std::string::size_type close = line.find(']');
        if (close == std::string::npos)
        {
            continue;
        }
        const std::string token = line.substr(1, close - 1);
        std::string rest = line.substr(close + 1);
        LLStringUtil::trim(rest);

        const EState state = SLAO::stateFromNotecardToken(token);
        if (state == SLAO::STATE_NONE)
        {
            std::string trimmed = token;
            LLStringUtil::trim(trimmed);
            if (!trimmed.empty() && mIgnoredSections.size() < 32 &&
                std::find(mIgnoredSections.begin(), mIgnoredSections.end(), trimmed) == mIgnoredSections.end())
            {
                mIgnoredSections.push_back(trimmed);
            }
            continue;
        }

        if (token.find('.') != std::string::npos)
        {
            saw_oracul = true;
        }
        else
        {
            saw_zhao = true;
        }

        if (rest.empty())
        {
            continue;
        }

        const char sep = (rest.find('|') != std::string::npos) ? '|' : ',';
        std::vector<std::string>& names = mNames[state];
        std::string::size_type start = 0;
        while (start <= rest.size())
        {
            std::string::size_type end = rest.find(sep, start);
            if (end == std::string::npos)
            {
                end = rest.size();
            }
            std::string name = rest.substr(start, end - start);
            LLStringUtil::trim(name);
            // Keep a notecard bounded no matter what it contains
            if (!name.empty() && names.size() < 256 &&
                std::find(names.begin(), names.end(), name) == names.end())
            {
                names.push_back(name);
            }
            start = end + 1;
        }
    }

    for (const auto& names : mNames)
    {
        if (!names.empty())
        {
            ++mStateCount;
            mAnimCount += (S32)names.size();
        }
    }
    mFormat = (saw_zhao && saw_oracul) ? "mixed" : (saw_oracul ? "Oracul" : "ZHAO-II");
    return mAnimCount > 0;
}

//-----------------------------------------------------------------------------
// Self-test: the notecard of a real AO HUD (OP AO, "pegasus" set), verbatim,
// with its quirks: commas with and without spaces, a trailing comma,
// trailing blanks, empty sections, a name with a space, and the same
// animation in several sections.
//-----------------------------------------------------------------------------

// static
bool SLAONotecard::selfTest(std::string& report)
{
    static const char* const CARD =
        "[ Crouching ] iwtcird\n"
        "[ CrouchWalking ] op_cwalk_roll\n"
        "[ Falling Down ] op_falling\n"
        "[ Flying ] op_flyP_fwd\n"
        "[ FlyingSlow ] op_flyP_fwd\n"
        "[ Hovering ] op_hoverP\n"
        "[ Hovering Down ] op_hoverP_down\n"
        "[ Hovering Up ] op_hoverP_up\n"
        "[ Jumping ] op_jump\n"
        "[ Landing ] op_softland\n"
        "[ PreJumping ] op_prejump\n"
        "[ Running ] op_runP_fwd \n"
        "[ Sitting ] op_sitobj_ovrme_tail_ULR, op_sitobj_default_tail_R, op_sitobj_siton_tail_LR\n"
        "[ Sitting on Ground ] op_sitg_flop_tail_DC, op_sitg_sidesit_tail_L, op_sitg_relaxed_tail_DL, op_sitg_default_tail_LR, op_sitg_lay_back_tail_DC, op_sitg_ovrme_tail_ULR\n"
        "[ Standing ] op_idle0001flinch, op_idle0001, op_idle0002, op_idle0003, op_idle0004, op_idle0005\n"
        "[ Standing Up ] op_standup\n"
        "[ Striding ] op_striding\n"
        "[ Soft Landing ] op_landing\n"
        "[ Taking Off ] op_prejump\n"
        "[ Turning Left ] op_turnleft\n"
        "[ Turning Right ] op_turnright\n"
        "[ Walking ] op_walkjovial, op_walkrestrained,op_walkystompy,\n"
        "[ Away ] op_sleepyaway\n"
        "[ Busy ] op_sleepyaway\n"
        "[ Typing ] op_typingsmile\n"
        "[ Walking Backward ] op_backup\n"
        "[ Swimming Up ] op_swimup\n"
        "[ Swimming Forward ] op_swimfoward\n"
        "[ Swimming Fast ] op_swimfast\n"
        "[ Swimming Idle ] op_swimidle\n"
        "[ Swimming Down ] op_swimdown\n"
        "[ Swimming Surface ] op_swimsurface\n"
        "[ Flying Left ] op_flyP_turnL\n"
        "[ Flying Right ] op_flyP_turnR\n"
        "[ Sea Walk ] \n"
        "[ Sea Run ] \n"
        "[ Running Left ] op_runP_turnL\n"
        "[ Running Right ] op_runP_turnR\n"
        "[ Deformer ] tpose bonetrans\n";

    struct Expect
    {
        EState      mState;
        const char* mNames; // '|'-separated, in order
    };
    static const Expect EXPECT[] =
    {
        { CROUCH, "iwtcird" },
        { CROUCH_WALK, "op_cwalk_roll" },
        { FALL, "op_falling" },
        { FLY, "op_flyP_fwd" },
        { FLY_SLOW, "op_flyP_fwd" },
        { HOVER, "op_hoverP" },
        { HOVER_DOWN, "op_hoverP_down" },
        { HOVER_UP, "op_hoverP_up" },
        { JUMP, "op_jump" },
        { LAND_SOFT, "op_softland|op_landing" },
        { PRE_JUMP, "op_prejump" },
        { RUN, "op_runP_fwd" },
        { SIT, "op_sitobj_ovrme_tail_ULR|op_sitobj_default_tail_R|op_sitobj_siton_tail_LR" },
        { SIT_GROUND, "op_sitg_flop_tail_DC|op_sitg_sidesit_tail_L|op_sitg_relaxed_tail_DL|op_sitg_default_tail_LR|op_sitg_lay_back_tail_DC|op_sitg_ovrme_tail_ULR" },
        { STAND, "op_idle0001flinch|op_idle0001|op_idle0002|op_idle0003|op_idle0004|op_idle0005" },
        { STAND_UP, "op_standup" },
        { STRIDE, "op_striding" },
        { TAKE_OFF, "op_prejump" },
        { TURN_LEFT, "op_turnleft" },
        { TURN_RIGHT, "op_turnright" },
        { WALK, "op_walkjovial|op_walkrestrained|op_walkystompy" },
        { AWAY, "op_sleepyaway" },
        { BUSY, "op_sleepyaway" },
        { TYPING, "op_typingsmile" },
        { WALK_BACK, "op_backup" },
        { SWIM_UP, "op_swimup" },
        { SWIM_FORWARD, "op_swimfoward" },
        { SWIM_FAST, "op_swimfast" },
        { FLOATING, "op_swimidle" },
        { SWIM_DOWN, "op_swimdown" },
        { SWIM_SURFACE, "op_swimsurface" },
        { FLY_LEFT, "op_flyP_turnL" },
        { FLY_RIGHT, "op_flyP_turnR" },
        { SEA_WALK, "" },
        { SEA_RUN, "" },
        { RUN_LEFT, "op_runP_turnL" },
        { RUN_RIGHT, "op_runP_turnR" },
        { ALWAYS, "tpose bonetrans" },
    };

    SLAONotecard card;
    card.parse(CARD);
    report.clear();
    if (!card.mIgnoredSections.empty())
    {
        report += "unrecognised sections:";
        for (const std::string& section : card.mIgnoredSections)
        {
            report += " [" + section + "]";
        }
        report += "; ";
    }
    for (const Expect& expect : EXPECT)
    {
        std::string got;
        for (const std::string& name : card.mNames[expect.mState])
        {
            got += (got.empty() ? "" : "|") + name;
        }
        if (got != expect.mNames)
        {
            report += stateFolderName(expect.mState) + ": got '" + got + "', want '" + expect.mNames + "'; ";
        }
    }
    if (report.empty())
    {
        report = llformat("all %d sections map (%s, %d animations in %d states)",
                          (S32)(sizeof(EXPECT) / sizeof(EXPECT[0])), card.mFormat.c_str(),
                          card.mAnimCount, card.mStateCount);
        return true;
    }
    return false;
}
