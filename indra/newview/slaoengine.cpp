/**
 * @file slaoengine.cpp
 * @brief Client-side animation overrider (#496): the runtime.
 *
 * How it works
 *  - The server tells the viewer which animations play on the agent
 *    (AvatarAnimation, with the object that started each one). The built-in
 *    state animations (stand, walk, fly, sit, ...) give the posture state.
 *  - For that state the AO asks the server to play the active set's
 *    replacement (AgentAnimation, the same request gestures use), and to stop
 *    the previous one, in a single message. Replacements have a higher
 *    priority than the built-ins; for the states where built-ins leak
 *    (hover arms, turns, landings, sits) the built-in is stopped as well.
 *  - Flicker: the request goes out as soon as the state is known: from the
 *    server's message, or one frame after a movement key changes for the
 *    ground and air locomotion states (prediction, corrected by the server
 *    within a second). The replacement also starts locally at once.
 *  - Seamless mode (SLAOSyncIntercept, #668): the server's list reaches the
 *    AO before the avatar starts its motions, and the built-ins the
 *    replacement covers are never started on our avatar (see "Seamless state
 *    changes" below).
 *  - Animations started by anything else (poseballs, furniture, collars,
 *    dance HUDs, gestures, server-side llSetAnimationOverride) are classified
 *    from their keyframe data once loaded: if they move the hips or legs, the
 *    idle states (stand, turn, hover, sit ...) pause until they stop.
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

#include "slaoengine.h"

#include <algorithm>
#include <iterator>

#include "llagent.h"
#include "llagentcamera.h"
#include "llanimationstates.h"
#include "llassetstorage.h"
#include "llcallbacklist.h"
#include "llfloaterreg.h"
#include "llinventorymodel.h"
#include "llhudeffectlookat.h"
#include "llkeyframemotion.h"
#include "llnotificationsutil.h"
#include "llrand.h"
#include "lltrans.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"
#include "message.h"
#include "rlvcommon.h"
#include "rlvhandler.h"
#include "slaosets.h"
#include "slswim.h"

using namespace SLAO;

namespace
{
    constexpr U32 MOVE_MASK = AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG |
                              AGENT_CONTROL_LEFT_POS | AGENT_CONTROL_LEFT_NEG |
                              AGENT_CONTROL_UP_POS | AGENT_CONTROL_UP_NEG |
                              AGENT_CONTROL_NUDGE_AT_POS | AGENT_CONTROL_NUDGE_AT_NEG |
                              AGENT_CONTROL_NUDGE_LEFT_POS | AGENT_CONTROL_NUDGE_LEFT_NEG |
                              AGENT_CONTROL_FAST_AT |
                              AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG |
                              AGENT_CONTROL_TURN_LEFT | AGENT_CONTROL_TURN_RIGHT;
    constexpr U32 HORIZONTAL_MASK = AGENT_CONTROL_AT_POS | AGENT_CONTROL_AT_NEG |
                                    AGENT_CONTROL_LEFT_POS | AGENT_CONTROL_LEFT_NEG |
                                    AGENT_CONTROL_NUDGE_AT_POS | AGENT_CONTROL_NUDGE_AT_NEG |
                                    AGENT_CONTROL_NUDGE_LEFT_POS | AGENT_CONTROL_NUDGE_LEFT_NEG |
                                    AGENT_CONTROL_FAST_AT;

    constexpr F64 TICK_SECS = 0.25;
    constexpr F64 PREDICT_WINDOW_SECS = 1.0;    // prediction wins until the server catches up
    constexpr F64 CONFIRM_TIMEOUT_SECS = 2.0;   // our animation should be in the server's list by then
    constexpr S32 MAX_RETRIES = 3;
    constexpr F64 RECENTLY_STOPPED_SECS = 3.0;
    constexpr F64 DEFAULT_STOP_MIN_GAP_SECS = 0.5;
    constexpr F64 DEFAULT_STOP_WINDOW_SECS = 10.0;
    constexpr size_t DEFAULT_STOP_MAX = 6;      // per window; then let the server win
    constexpr F64 PRELOAD_TIMEOUT_SECS = 45.0;
    constexpr F64 HOLD_RELEASE_SECS = 15.0;     // holding object gone this long -> released
    constexpr size_t MAX_HOLDS = 64;
    constexpr size_t MAX_CLASS_CACHE = 512;
    constexpr size_t MAX_SEATS = 64;
    constexpr size_t MAX_BAD = 256;
    constexpr size_t MAX_PRELOAD = 256;
    constexpr size_t MAX_ALWAYS = 32;
    constexpr F64 TAKE_OFF_SECS = 1.5;
    constexpr F32 POSE_MIN_ANGLE_RAD = 3.f * DEG_TO_RAD;  // smaller rotations count as rest
    constexpr F32 YAW_START_DEG_PER_SEC = 45.f;
    constexpr F32 YAW_STOP_DEG_PER_SEC = 20.f;
    constexpr F64 YAW_STOP_HOLD_SECS = 0.25;          // hover up this soon after a ground state
    constexpr F32 SURFACE_ABOVE = 0.5f;         // metres above the water still counted as surface
    constexpr F32 SURFACE_BELOW = 0.75f;        // metres below the water still counted as surface
    constexpr F32 SEA_FLOOR_DEPTH = 1.0f;       // walking this deep is walking on the sea floor
    constexpr size_t MAX_FETCH_REQUESTS = 1024;   // per session
    constexpr F64 SCRIPTED_AO_REACTION_SECS = 1.5;
    constexpr F64 SCRIPTED_AO_WINDOW_SECS = 120.0;
    constexpr S32 SCRIPTED_AO_HITS = 4;
    constexpr size_t SCRIPTED_AO_MIN_ANIMS = 2;
    constexpr size_t MAX_SCRIPTED_AO_SCORES = 32;
    constexpr S32 PROTOCOL_VERSION = 1;
    constexpr F64 STATUS_REPLY_WINDOW_SECS = 10.0;  // status replies (avatar chat): at most
    constexpr S32 STATUS_REPLY_MAX = 10;            // this many per window

    F64 now()
    {
        return LLTimer::getElapsedSeconds();
    }

    enum EFamily { FAMILY_NONE, FAMILY_GROUND, FAMILY_AIR };

    EFamily family(EState state)
    {
        switch (state)
        {
        case STAND: case WALK: case RUN: case CROUCH: case CROUCH_WALK:
            return FAMILY_GROUND;
        case FLY: case FLY_SLOW: case HOVER: case HOVER_UP: case HOVER_DOWN:
            return FAMILY_AIR;
        default:
            return FAMILY_NONE;
        }
    }

    // States that predict the same thing (walk vs run, fly vs fly slow are
    // the server's call)
    S32 coarse(EState state)
    {
        switch (state)
        {
        case WALK: case RUN: return 1;
        case FLY: case FLY_SLOW: return 2;
        default: return 100 + (S32)state;
        }
    }

    // Joints that make an animation a full-body pose (as opposed to a hand,
    // face, arm or tail animation that can play on top of a stand).
    bool is_body_joint(const std::string& name)
    {
        return name == "mPelvis" || name == "mTorso" ||
               name == "mHipLeft" || name == "mHipRight" ||
               name == "mKneeLeft" || name == "mKneeRight" ||
               name == "mAnkleLeft" || name == "mAnkleRight";
    }

    bool local_playback()
    {
        static LLCachedControl<bool> sLocal(gSavedSettings, "SLAOLocalPlayback", true);
        return sLocal && isAgentAvatarValid();
    }

    // Built-ins that stay on our avatar under a replacement in sync mode
    // (SLAOSyncIntercept). The pre-jump, landings and stand-up end with their
    // own motion: the viewer tells the server the state is done when the
    // motion stops (LLAgent::onAnimStop), so they keep playing with the
    // replacement layered on top. The sit is the base pose under the sit
    // replacement (covers loading and loop points, for observers too); the
    // ground sit's motion is the viewer's own sitting state; typing is an
    // overlay.
    bool keeps_stock_locally(EState state)
    {
        switch (state)
        {
        case PRE_JUMP: case LAND_SOFT: case LAND_MEDIUM: case STAND_UP:
        case SIT: case SIT_GROUND: case TYPING:
            return true;
        default:
            return false;
        }
    }

    bool is_transient(EState state)
    {
        return state == PRE_JUMP || state == LAND_SOFT || state == LAND_MEDIUM || state == STAND_UP;
    }

    // Sets a flag for the lifetime of the scope, restoring what it was
    struct ScopedFlag
    {
        explicit ScopedFlag(bool& flag) : mFlag(flag), mOld(flag) { mFlag = true; }
        ~ScopedFlag() { mFlag = mOld; }
        bool& mFlag;
        bool mOld;
    };

    constexpr size_t MAX_OWN_STOPPED = 64;

    std::string attachment_name(const LLViewerObject* object)
    {
        if (!object)
        {
            return std::string();
        }
        const LLViewerObject* root = object->getRootEdit();
        if (root && root->isAttachment())
        {
            if (const LLViewerInventoryItem* item = gInventory.getItem(root->getAttachmentItemID()))
            {
                return item->getName();
            }
        }
        return std::string();
    }
}

SLAOEngine::SLAOEngine()
{
    std::fill(std::begin(mIndex), std::end(mIndex), 0);
    std::fill(std::begin(mManualPick), std::end(mManualPick), false);
    std::fill(std::begin(mEverPicked), std::end(mEverPicked), false);
}

SLAOEngine::~SLAOEngine()
{
}

//-----------------------------------------------------------------------------
// Lifetime
//-----------------------------------------------------------------------------

void SLAOEngine::init()
{
    if (mInitialized)
    {
        return;
    }
    mInitialized = true;

    mCurrentSetID = LLUUID(gSavedPerAccountSettings.getString("SLAOCurrentSet"));

    mSetsConnection = SLAOSets::instance().setChangedCallback([this]() { onSetsChanged(); });
    mRegionConnection = gAgent.addRegionChangedCallback([this]() { onRegionChanged(); });
    if (LLControlVariable* control = gSavedPerAccountSettings.getControl("SLAOEnabled"))
    {
        mEnabledConnection = control->getSignal()->connect([this](LLControlVariable*, const LLSD&, const LLSD&)
            {
                onSettingChanged();
            });
    }

    doOnIdleRepeating([]() -> bool
        {
            if (!SLAOEngine::instanceExists())
            {
                return true;
            }
            SLAOEngine& self = SLAOEngine::instance();
            if (!self.mInitialized)
            {
                return true;
            }
            self.onIdle();
            return false;
        });

    LL_INFOS() << "AO initialized, enabled=" << isEnabled() << " set=" << mCurrentSetID << LL_ENDL;
}

void SLAOEngine::shutdown()
{
    mInitialized = false;
    mSuppressed.clear();
    mSetsConnection.disconnect();
    mRegionConnection.disconnect();
    mEnabledConnection.disconnect();
}

bool SLAOEngine::isEnabled() const
{
    return gSavedPerAccountSettings.controlExists("SLAOEnabled") && gSavedPerAccountSettings.getBOOL("SLAOEnabled");
}

void SLAOEngine::setEnabled(bool enabled)
{
    if (enabled && mUserPaused)
    {
        mUserPaused = false;
    }
    if (enabled && !getCurrentSet() && SLAOSets::instance().isInventoryScanned())
    {
        // Switching on without a set: use the first one there is
        const std::vector<const SLAOSet*> sets = SLAOSets::instance().getSortedSets();
        if (!sets.empty())
        {
            selectSet(sets.front()->mFolderID);
        }
    }
    gSavedPerAccountSettings.setBOOL("SLAOEnabled", enabled); // fires onSettingChanged
    update();
    notifyChanged();
}

void SLAOEngine::setUserPaused(bool paused)
{
    if (mUserPaused != paused)
    {
        mUserPaused = paused;
        update();
        notifyChanged();
    }
}

void SLAOEngine::onSettingChanged()
{
    update();
    notifyChanged();
}

//-----------------------------------------------------------------------------
// Sets
//-----------------------------------------------------------------------------

const SLAOSet* SLAOEngine::getCurrentSet() const
{
    return SLAOSets::instance().getSet(mCurrentSetID);
}

bool SLAOEngine::overridesWalk() const
{
    if (!mInitialized)
    {
        return false;
    }
    const SLAOSet* set = getCurrentSet();
    return set && pauseFor(WALK) == PAUSE_NONE &&
           (set->resolveState(WALK) != STATE_NONE || set->resolveState(WALK_BACK) != STATE_NONE);
}

void SLAOEngine::selectSet(const LLUUID& set_id)
{
    if (set_id == mCurrentSetID)
    {
        return;
    }
    mCurrentSetID = set_id;
    gSavedPerAccountSettings.setString("SLAOCurrentSet", set_id.asString());
    std::fill(std::begin(mIndex), std::end(mIndex), 0);
    std::fill(std::begin(mManualPick), std::end(mManualPick), false);
    std::fill(std::begin(mEverPicked), std::end(mEverPicked), false);
    // Force a fresh pick; apply() swaps old for new in one message
    mMain.mState = STATE_NONE;
    mOverlay.mState = STATE_NONE;
    SLAOSets::instance().updateSnapshot(set_id);
    preloadSet();
    update();
    notifyChanged();
}

bool SLAOEngine::selectSetByName(const std::string& name)
{
    const SLAOSet* set = SLAOSets::instance().findSetByName(name);
    if (!set)
    {
        return false;
    }
    selectSet(set->mFolderID);
    return true;
}

void SLAOEngine::cycleSet(S32 delta)
{
    const std::vector<const SLAOSet*> sets = SLAOSets::instance().getSortedSets();
    if (sets.empty())
    {
        return;
    }
    S32 index = -1;
    for (S32 i = 0; i < (S32)sets.size(); ++i)
    {
        if (sets[i]->mFolderID == mCurrentSetID)
        {
            index = i;
            break;
        }
    }
    const S32 count = (S32)sets.size();
    index = (index < 0) ? 0 : ((index + delta) % count + count) % count;
    selectSet(sets[index]->mFolderID);
}

void SLAOEngine::onSetsChanged()
{
    // The active set may have gained or lost animations, or vanished
    const SLAOSet* set = getCurrentSet();
    if (set && !set->mFromSnapshot)
    {
        SLAOSets::instance().updateSnapshot(mCurrentSetID);
    }
    preloadSet();
    update();
    notifyChanged();
}

bool SLAOEngine::getSitOverride() const
{
    const SLAOSet* set = getCurrentSet();
    return set && set->mSitOverride;
}

void SLAOEngine::setSitOverride(bool enabled)
{
    const SLAOSet* set = getCurrentSet();
    if (!set)
    {
        return;
    }
    SLAOSets::instance().setSetOptions(set->mFolderID, enabled, set->mSmartSit,
                                       set->mNoStandsInMouselook, set->mNoStandsWhileTyping);
    update();
    notifyChanged();
}

void SLAOEngine::nextAnim(S32 delta)
{
    const SLAOSet* set = getCurrentSet();
    if (!set)
    {
        return;
    }
    EState state = (mMain.mState != STATE_NONE) ? mMain.mState : set->resolveState(STAND);
    if (state == STATE_NONE)
    {
        return;
    }
    const S32 count = (S32)set->mAnims[state].size();
    if (count < 2)
    {
        return;
    }
    if (set->mStateConfig[state].mRandom && delta > 0)
    {
        const S32 current = mIndex[state];
        mIndex[state] = (current + 1 + (S32)ll_rand(count - 1)) % count;
    }
    else
    {
        mIndex[state] = ((mIndex[state] + delta) % count + count) % count;
    }
    mManualPick[state] = true;
    mLastCycleAt = now();
    update();
    notifyChanged();
}

//-----------------------------------------------------------------------------
// Animation state input
//-----------------------------------------------------------------------------

void SLAOEngine::onSelfAnimations(const std::vector<std::pair<LLUUID, LLUUID>>& anims)
{
    if (!mInitialized)
    {
        return;
    }

    std::map<LLUUID, LLUUID> next;
    for (const auto& [anim, source] : anims)
    {
        next[anim] = source;
    }
    std::vector<std::pair<LLUUID, LLUUID>> added;
    for (const auto& [anim, source] : next)
    {
        if (!mSignaled.count(anim))
        {
            added.emplace_back(anim, source);
        }
    }
    mSignaled.swap(next);

    // State from the list alone; if the server moved to another state, a
    // default we stopped no longer needs restoring.
    EState from_list = STATE_NONE;
    // Still typing after we stopped the built-in typing animation
    mTypingSignaled = mStoppedTyping && (gAgent.getRenderState() & AGENT_STATE_TYPING);
    for (const auto& [anim, source] : mSignaled)
    {
        const EState state = stateForDefaultAnim(anim);
        if (state == TYPING)
        {
            mTypingSignaled = true;
        }
        else if (state != STATE_NONE && stateRank(state) > stateRank(from_list))
        {
            from_list = state;
        }
    }
    if (from_list != STATE_NONE)
    {
        // The server is in a listed state: built-ins of other states we
        // stopped are gone for good; one of this state may be re-asserted
        // (then it is stopped again, rate-limited, in apply()).
        for (auto it = mStoppedDefaults.begin(); it != mStoppedDefaults.end(); )
        {
            it = (it->second != from_list) ? mStoppedDefaults.erase(it) : std::next(it);
        }
    }
    if (mStoppedAway && !gAgent.getAFK())
    {
        mStoppedAway = false;
    }

    const EState old_state = mSignaledState;
    mSignaledState = computeSignaledState();
    if (mSignaledState != old_state)
    {
        mSignaledChangedAt = now();
        mStateChangedAt = mSignaledChangedAt;
        if (family(mSignaledState) == FAMILY_AIR && family(old_state) != FAMILY_AIR &&
            old_state != FALL && old_state != JUMP)
        {
            mAirEnteredAt = mSignaledChangedAt;
        }
        if (mSignaledState == SIT)
        {
            mSitSignaledAt = mSignaledChangedAt;
        }
    }
    updateSeat();

    // Our own animations showing up confirm the requests
    for (Channel* channel : { &mMain, &mOverlay })
    {
        if (channel->mAnim.notNull() && mSignaled.count(channel->mAnim))
        {
            channel->mConfirmed = true;
        }
    }

    scanForeign();
    for (const auto& [anim, source] : added)
    {
        noteScriptedAOActivity(anim, source);
    }
    update();
}

EState SLAOEngine::computeSignaledState() const
{
    EState best = STATE_NONE;
    for (const auto& [anim, source] : mSignaled)
    {
        const EState state = stateForDefaultAnim(anim);
        if (state != STATE_NONE && state != TYPING && stateRank(state) > stateRank(best))
        {
            best = state;
        }
    }
    if (best != STATE_NONE || mStoppedDefaults.empty())
    {
        return best;
    }
    // We removed the built-ins ourselves: the state has not changed (the
    // server adds a built-in again when it changes). A stopped turn lasts only
    // while the avatar is still turning; the stand underneath carries on.
    const bool sitting = isAgentAvatarValid() && gAgentAvatarp->isSitting();
    for (const auto& [id, state] : mStoppedDefaults)
    {
        if ((state == TURN_LEFT || state == TURN_RIGHT) && !isTurning())
        {
            continue;
        }
        if (isSitState(state) && !sitting)
        {
            continue;
        }
        if (stateRank(state) > stateRank(best))
        {
            best = state;
        }
    }
    if (best == STATE_NONE && !sitting && !gAgent.getFlying())
    {
        // Only stopped turns were left: the avatar stands
        for (const auto& [id, state] : mStoppedDefaults)
        {
            if (state == TURN_LEFT || state == TURN_RIGHT)
            {
                best = STAND;
                break;
            }
        }
    }
    return best;
}

bool SLAOEngine::isTurning() const
{
    return mYawTurn != 0 ||
           (mLastControlMask & (AGENT_CONTROL_YAW_POS | AGENT_CONTROL_YAW_NEG |
                                AGENT_CONTROL_TURN_LEFT | AGENT_CONTROL_TURN_RIGHT)) != 0;
}

void SLAOEngine::trackYaw()
{
    // Turning from the avatar's real rotation (mouselook steering turns it
    // without any turn key). Hysteresis: start above 45 deg/s, stop after
    // 0.25 s below 20 deg/s.
    const F64 t = now();
    const LLVector3 at = gAgent.getFrameAgent().getAtAxis();
    const F32 yaw = atan2f(at.mV[VY], at.mV[VX]);
    if (mYawTime > 0.0 && t - mYawTime > 0.0005 && t - mYawTime < 0.5)
    {
        F32 delta = yaw - mLastYaw;
        while (delta > F_PI)
        {
            delta -= F_TWO_PI;
        }
        while (delta < -F_PI)
        {
            delta += F_TWO_PI;
        }
        const F32 rate = delta / (F32)(t - mYawTime);
        mYawRate = 0.5f * mYawRate + 0.5f * rate; // smooth frame jitter
    }
    else
    {
        mYawRate = 0.f;
    }
    mLastYaw = yaw;
    mYawTime = t;

    const F32 deg = mYawRate * RAD_TO_DEG;
    S32 turn = mYawTurn;
    if (fabsf(deg) > YAW_START_DEG_PER_SEC)
    {
        turn = (deg > 0.f) ? 1 : -1; // positive yaw is a left turn
        mYawQuietSince = 0.0;
    }
    else if (fabsf(deg) < YAW_STOP_DEG_PER_SEC)
    {
        if (mYawTurn != 0)
        {
            if (mYawQuietSince == 0.0)
            {
                mYawQuietSince = t;
            }
            else if (t - mYawQuietSince > YAW_STOP_HOLD_SECS)
            {
                turn = 0;
            }
        }
    }
    else
    {
        mYawQuietSince = 0.0;
    }
    if (turn != mYawTurn)
    {
        mYawTurn = turn;
        if (isEnabled())
        {
            update();
        }
    }
}

void SLAOEngine::onControls(U32 control_flags)
{
    if (!mInitialized)
    {
        return;
    }
    if (isAgentAvatarValid() && !gAgentAvatarp->isSitting())
    {
        trackYaw();
    }
    else
    {
        mYawTurn = 0;
        mYawTime = 0.0;
    }
    const U32 mask = control_flags & MOVE_MASK;
    const bool flying = gAgent.getFlying();
    if (mask == mLastControlMask && flying == mLastFlying)
    {
        return;
    }
    mLastControlMask = mask;
    mLastFlying = flying;
    mControlsChangedAt = now();

    static LLCachedControl<bool> sPredict(gSavedSettings, "SLAOPredictLocomotion", true);
    if (sPredict && isEnabled())
    {
        update();
    }
}

EState SLAOEngine::predictState() const
{
    if (!isAgentAvatarValid() || gAgentAvatarp->isSitting())
    {
        return STATE_NONE;
    }
    const U32 flags = mLastControlMask;
    const bool horizontal = (flags & HORIZONTAL_MASK) != 0;
    if (mLastFlying)
    {
        if (horizontal)
        {
            return FLY;
        }
        if (flags & AGENT_CONTROL_UP_POS)
        {
            return HOVER_UP;
        }
        if (flags & AGENT_CONTROL_UP_NEG)
        {
            return HOVER_DOWN;
        }
        return HOVER;
    }
    if (flags & AGENT_CONTROL_UP_POS)
    {
        return STATE_NONE; // jumping: leave it to the server
    }
    if (flags & AGENT_CONTROL_UP_NEG)
    {
        return horizontal ? CROUCH_WALK : CROUCH;
    }
    if (horizontal)
    {
        return gAgent.getRunning() ? RUN : WALK;
    }
    return STAND;
}

EState SLAOEngine::effectiveState()
{
    EState state = mSignaledState;

    static LLCachedControl<bool> sPredict(gSavedSettings, "SLAOPredictLocomotion", true);
    const F64 t = now();
    if (sPredict && (t - mControlsChangedAt) < PREDICT_WINDOW_SECS && mSignaledChangedAt < mControlsChangedAt)
    {
        const EState predicted = predictState();
        // Ground <-> air only when the viewer itself switched flight (the
        // server may refuse a flight the user asks for; ours was checked)
        const bool own_transition = syncInterceptEnabled() && (t - mOwnTransitionAt) < PREDICT_WINDOW_SECS &&
                                    family(state) != FAMILY_NONE;
        if (predicted != STATE_NONE && family(predicted) != FAMILY_NONE &&
            (family(predicted) == family(state) || own_transition) && coarse(predicted) != coarse(state))
        {
            state = predicted;
        }
    }

    // States the server has no animation for, derived from a base state.
    // Each falls back to its base state when the set has none (slaoset.cpp).
    const U32 keys = mLastControlMask;
    const bool back = (keys & (AGENT_CONTROL_AT_NEG | AGENT_CONTROL_NUDGE_AT_NEG)) &&
                      !(keys & (AGENT_CONTROL_AT_POS | AGENT_CONTROL_NUDGE_AT_POS));
    const bool turning_left = (keys & (AGENT_CONTROL_YAW_POS | AGENT_CONTROL_TURN_LEFT)) ||
                              mSignaled.count(ANIM_AGENT_TURNLEFT) || mYawTurn > 0;
    const bool turning_right = (keys & (AGENT_CONTROL_YAW_NEG | AGENT_CONTROL_TURN_RIGHT)) ||
                               mSignaled.count(ANIM_AGENT_TURNRIGHT) || mYawTurn < 0;

    // Jump: the up key on the ground starts the pre-jump before the server
    // says so (only when the set has one, so the stand never drops out)
    if (sPredict && (t - mControlsChangedAt) < PREDICT_WINDOW_SECS && mSignaledChangedAt < mControlsChangedAt &&
        family(state) == FAMILY_GROUND && (keys & AGENT_CONTROL_UP_POS) && !gAgent.getFlying() &&
        isAgentAvatarValid() && !gAgentAvatarp->isSitting())
    {
        const SLAOSet* jump_set = getCurrentSet();
        if (jump_set && jump_set->resolveState(PRE_JUMP) != STATE_NONE)
        {
            state = PRE_JUMP;
        }
    }
    // (moveAt sets FAST_AT for the back key too)
    const bool fast = ((keys & AGENT_CONTROL_FAST_AT) && (keys & AGENT_CONTROL_AT_POS)) || gAgent.getRunning();

    switch (state)
    {
    case WALK:
        // The server plays the normal walk backwards too. With "Walk backwards
        // facing forward" off the avatar turns round to face its travel (same as
        // running backwards), so it plays the ordinary forward walk (#922).
        if (back && !gAgent.getFlying() && gSavedSettings.getBOOL("SLWalkBackwardsFacingForward"))
        {
            state = WALK_BACK;
        }
        break;
    case RUN:
        if (turning_left != turning_right)
        {
            state = turning_left ? RUN_LEFT : RUN_RIGHT;
        }
        break;
    case FLY:
    case FLY_SLOW:
        if (turning_left != turning_right)
        {
            state = turning_left ? FLY_LEFT : FLY_RIGHT;
        }
        break;
    case HOVER_UP:
        // Leaving the ground: the first moments of going up after a ground state
        if (t - mAirEnteredAt < TAKE_OFF_SECS)
        {
            state = TAKE_OFF;
        }
        break;
    case STAND:
        // Turning in place, however it is steered (keys, mouselook)
        if (turning_left != turning_right)
        {
            state = turning_left ? TURN_LEFT : TURN_RIGHT;
        }
        else if (gAgent.isDoNotDisturb())
        {
            state = BUSY;
        }
        else if (gAgent.getAFK())
        {
            state = AWAY;
        }
        break;
    default:
        break;
    }

    // Water. SL has no swimming; depth is the region water height minus the
    // agent's position (about hip height):
    //  - in the air states within SURFACE_ABOVE above to SURFACE_BELOW below
    //    the surface: hovering is Swimming Surface, moving is Swimming Forward;
    //  - deeper: hover -> Swimming Idle (Floating), fly -> Swimming Forward
    //    (Swimming Fast when running/fast), up/down -> Swimming Up/Down;
    //  - walking or running more than SEA_FLOOR_DEPTH under water: Sea Walk/Run.
    // Simulated swimming: the float controller's slow vertical nudges keep
    // the avatar at the surface; that is floating, not swimming up or down
    if (SLSwimController::instance().isSwimming() && (state == HOVER_UP || state == HOVER_DOWN) &&
        !(keys & (AGENT_CONTROL_UP_POS | AGENT_CONTROL_UP_NEG)))
    {
        state = HOVER;
    }

    if (isAgentAvatarValid() && gAgent.getRegion() && state != TAKE_OFF)
    {
        const F32 depth = gAgent.getRegion()->getWaterHeight() - gAgent.getPositionAgent().mV[VZ];
        const bool air = family(state) == FAMILY_AIR || state == FLY_LEFT || state == FLY_RIGHT;
        if (air && depth > -SURFACE_ABOVE)
        {
            if (depth < SURFACE_BELOW)
            {
                state = (state == HOVER) ? SWIM_SURFACE : (state == HOVER_UP || state == HOVER_DOWN) ? state : SWIM_FORWARD;
            }
            else
            {
                switch (state)
                {
                case HOVER: state = FLOATING; break;
                case HOVER_UP: state = SWIM_UP; break;
                case HOVER_DOWN: state = SWIM_DOWN; break;
                default: state = fast ? SWIM_FAST : SWIM_FORWARD; break;
                }
            }
        }
        else if (depth > SEA_FLOOR_DEPTH)
        {
            if (state == WALK || state == WALK_BACK || state == STRIDE)
            {
                state = SEA_WALK;
            }
            else if (state == RUN || state == RUN_LEFT || state == RUN_RIGHT)
            {
                state = SEA_RUN;
            }
        }
    }
    return state;
}

//-----------------------------------------------------------------------------
// Decisions
//-----------------------------------------------------------------------------

SLAOEngine::EPause SLAOEngine::pauseFor(EState state) const
{
    if (!isEnabled())
    {
        return PAUSE_DISABLED;
    }
    if (mUserPaused)
    {
        return PAUSE_USER;
    }
    const SLAOSet* set = getCurrentSet();
    if (!set)
    {
        return SLAOSets::instance().isInventoryScanned() ? PAUSE_NO_SET : PAUSE_LOADING;
    }
    if (!isAgentAvatarValid() || !gAgent.getRegion())
    {
        return PAUSE_LOADING;
    }
    if (isScriptedAOWorn())
    {
        return PAUSE_SCRIPTED_AO;
    }

    U32 holds = 0;
    for (const auto& [id, hold] : mHolds)
    {
        holds |= hold.mMask;
    }
    if (holds & HOLD_ALL)
    {
        return PAUSE_SCRIPT_HOLD;
    }
    if ((holds & HOLD_STAND) && isIdleState(state) && !isSitState(state))
    {
        return PAUSE_SCRIPT_HOLD;
    }
    if ((holds & HOLD_SIT) && isSitState(state))
    {
        return PAUSE_SCRIPT_HOLD;
    }

    static LLCachedControl<bool> sPauseForScripts(gSavedSettings, "SLAOPauseForScriptAnimations", true);
    static LLCachedControl<F32> sSitDelay(gSavedSettings, "SLAOSmartSitDelay", 0.75f);

    if (state == SIT)
    {
        if (!set->mSitOverride)
        {
            return PAUSE_SIT_OFF;
        }
        const bool rlv_locked = rlv_handler_t::isEnabled() && gRlvHandler.hasBehaviour(RLV_BHVR_UNSIT);
        if (set->mSmartSit || rlv_locked)
        {
            const EPause reason = set->mSmartSit ? PAUSE_SMART_SIT : PAUSE_RLV_SIT;
            if (mSeatAnimates || mBodyTakeover || mAnimatingSeats.count(mSeatID))
            {
                return reason;
            }
            // The wait starts with the sit animation, which can arrive before
            // the seat object is known
            if ((now() - llmax(mSitSignaledAt, mSatAt)) < (F64)sSitDelay)
            {
                return PAUSE_SIT_WAIT;
            }
        }
        return PAUSE_NONE;
    }
    if (state == SIT_GROUND)
    {
        if (set->mSmartSit && mBodyTakeover)
        {
            return PAUSE_SMART_SIT;
        }
        return PAUSE_NONE;
    }
    if (isIdleState(state) && mBodyTakeover && sPauseForScripts)
    {
        return mServerOverride ? PAUSE_SERVER_AO : PAUSE_SCRIPT_ANIM;
    }
    if (state == STAND || state == TURN_LEFT || state == TURN_RIGHT)
    {
        if (set->mNoStandsInMouselook && gAgentCamera.cameraMouselook())
        {
            return PAUSE_MOUSELOOK;
        }
        if (set->mNoStandsWhileTyping && mTypingSignaled)
        {
            return PAUSE_TYPING;
        }
    }
    return PAUSE_NONE;
}

LLUUID SLAOEngine::pickAnim(EState state, bool entering)
{
    const SLAOSet* set = getCurrentSet();
    if (!set || state <= STATE_NONE || state >= STATE_COUNT)
    {
        return LLUUID::null;
    }
    const std::vector<SLAOAnim>& anims = set->mAnims[state];
    const S32 count = (S32)anims.size();
    if (count == 0)
    {
        return LLUUID::null;
    }
    S32& index = mIndex[state];
    const SLAOStateConfig& config = set->mStateConfig[state];
    if (entering && !config.mCycle)
    {
        // Cycle off: the first animation of the list, unless the user picked another
        if (!mManualPick[state])
        {
            index = 0;
        }
    }
    else if (entering && count > 1 && config.mPickOnEntry)
    {
        // Cycle on, change only on entry: the next one, or a random one
        // (never the same twice in a row); the first entry plays the first
        // in order, or any with Random
        if (config.mRandom)
        {
            index = mEverPicked[state] ? (index + 1 + (S32)ll_rand(count - 1)) % count : (S32)ll_rand(count);
        }
        else if (mEverPicked[state])
        {
            index = index + 1;
        }
    }
    // Cycle on with the timer: entry keeps the position; the timer steps it
    if (entering)
    {
        mEverPicked[state] = true;
    }
    index = ((index % count) + count) % count;
    for (S32 k = 0; k < count; ++k)
    {
        const S32 i = (index + k) % count;
        if (!mBadAnims.count(anims[i].mAssetID))
        {
            index = i;
            return anims[i].mAssetID;
        }
    }
    return LLUUID::null;
}

void SLAOEngine::update()
{
    if (!mInitialized)
    {
        return;
    }
    // Our own starts and stops of built-ins below pass the motion hooks.
    // (No early return after this point: syncLocalStock() at the end must run.)
    ScopedFlag in_update(mInUpdate);

    mEffectiveState = effectiveState();
    const SLAOSet* set = getCurrentSet();

    // Posture channel
    EPause pause = PAUSE_NONE;
    EState resolved = STATE_NONE;
    LLUUID anim;
    if (mEffectiveState != STATE_NONE || !isEnabled() || mUserPaused || !set)
    {
        pause = pauseFor(mEffectiveState);
    }
    if (pause == PAUSE_NONE && set && mEffectiveState != STATE_NONE)
    {
        resolved = set->resolveState(mEffectiveState);
        if (resolved == STATE_NONE)
        {
            pause = PAUSE_EMPTY;
        }
        else
        {
            const bool entering = (resolved != mMain.mState);
            if (entering)
            {
                mStateEnteredAt = now();
                mLastCycleAt = mStateEnteredAt;
            }
            anim = pickAnim(resolved, entering);
            if (anim.isNull())
            {
                pause = PAUSE_EMPTY;
                resolved = STATE_NONE;
            }
        }
    }
    // A random idle plays in place of the stand (or sit) while it lasts
    if (mIdleAnim.notNull())
    {
        if (pause == PAUSE_NONE && set && isIdleHost(resolved))
        {
            anim = mIdleAnim;
        }
        else
        {
            endIdle(false);
        }
    }
    mPauseReason = pause;
    apply(mMain, resolved, anim, true, false);
    updateAlways();
    updateHeadHold();

    // Typing overlay
    EState overlay_state = STATE_NONE;
    LLUUID overlay_anim;
    if (mTypingSignaled && set && set->hasAnims(TYPING) && pauseFor(TYPING) == PAUSE_NONE)
    {
        overlay_state = TYPING;
        overlay_anim = pickAnim(TYPING, mOverlay.mState != TYPING);
    }
    apply(mOverlay, overlay_state, overlay_anim, false, false);

    // The built-in typing animation under our typing replacement
    std::vector<std::pair<LLUUID, bool>> typing_requests;
    if (overlay_anim.notNull())
    {
        if (stopSystemAnimsEnabled() && mSignaled.count(ANIM_AGENT_TYPE) &&
            mayStopBuiltin(ANIM_AGENT_TYPE, typing_requests))
        {
            mStoppedTyping = true;
        }
    }
    else if (mStoppedTyping)
    {
        mStoppedTyping = false;
        if ((gAgent.getRenderState() & AGENT_STATE_TYPING) && !mSignaled.count(ANIM_AGENT_TYPE))
        {
            typing_requests.emplace_back(ANIM_AGENT_TYPE, true);
        }
    }
    sendRequests(typing_requests);

    syncLocalStock();
    notifyChanged();
}

void SLAOEngine::apply(Channel& channel, EState state, const LLUUID& anim, bool main_channel, bool)
{
    std::vector<std::pair<LLUUID, bool>> requests;
    const F64 t = now();

    if (channel.mAnim != anim)
    {
        if (anim.notNull())
        {
            requests.emplace_back(anim, true);
            mOwnStopped.erase(anim);
            if (local_playback())
            {
                gAgentAvatarp->startMotion(anim);
            }
            if (!LLKeyframeDataCache::getKeyframeData(anim) && mPreloading.size() < MAX_PRELOAD)
            {
                mPreloading.emplace(anim, t);
            }
            if (isLowPriority(anim) && !mWarnedLowPriority.count(anim) && mWarnedLowPriority.size() < 256)
            {
                mWarnedLowPriority.insert(anim);
                LL_WARNS() << "AO animation " << anim << " has a low priority; it blends with the built-in "
                           << "unless \"Stop system animations\" is on" << LL_ENDL;
            }
        }
        if (channel.mAnim.notNull())
        {
            requests.emplace_back(channel.mAnim, false);
            if (local_playback())
            {
                gAgentAvatarp->stopMotion(channel.mAnim);
            }
            mRecentlyStopped[channel.mAnim] = t;
            noteOwnStop(channel.mAnim);
        }
        LL_DEBUGS("SLAO") << (main_channel ? "posture" : "overlay") << " " << stateLabel(state) << ": "
                          << channel.mAnim << " -> " << anim << LL_ENDL;
        channel.mAnim = anim;
        channel.mSentAt = t;
        channel.mRetries = 0;
        channel.mConfirmed = anim.notNull() && mSignaled.count(anim);
    }
    channel.mState = state;

    if (main_channel)
    {
        if (anim.notNull())
        {
            stopSystemAnims(state, requests);
        }
        else
        {
            restoreSystemAnims(requests);
        }
    }

    sendRequests(requests);
}

bool SLAOEngine::stopSystemAnimsEnabled() const
{
    static LLCachedControl<bool> sStopDefaults(gSavedSettings, "SLAOStopDefaultAnimations", true);
    const SLAOSet* set = getCurrentSet();
    return sStopDefaults && set && set->mStopSystemAnims;
}

bool SLAOEngine::mayStopBuiltin(const LLUUID& builtin, std::vector<std::pair<LLUUID, bool>>& requests)
{
    // Rate limit per built-in: the server may re-assert one (walk, stand) on
    // its state updates; stop it again, but never flood the region.
    const F64 t = now();
    std::vector<F64>& times = mDefaultStopTimes[builtin];
    times.erase(std::remove_if(times.begin(), times.end(),
        [t](F64 when) { return t - when > DEFAULT_STOP_WINDOW_SECS; }), times.end());
    const bool too_soon = !times.empty() && (t - times.back()) < DEFAULT_STOP_MIN_GAP_SECS;
    if (too_soon || times.size() >= DEFAULT_STOP_MAX)
    {
        return false;
    }
    times.push_back(t);
    requests.emplace_back(builtin, false);
    if (local_playback())
    {
        gAgentAvatarp->stopMotion(builtin);
    }
    if (mDefaultStopTimes.size() > 64)
    {
        mDefaultStopTimes.clear();
    }
    return true;
}

void SLAOEngine::stopSystemAnims(EState state, std::vector<std::pair<LLUUID, bool>>& requests)
{
    // While a replacement plays, every posture built-in in the server's list
    // is under it: stop them all so none leaks through on joints ours leaves
    // alone or on priority ties. Exceptions are in the state table
    // (stopsDefaultAnim): the ground sit, whose removal makes the viewer stand
    // the avatar up. Typing is handled by the overlay; DND is never touched.
    if (!stopSystemAnimsEnabled() || !stopsDefaultAnim(state))
    {
        return;
    }
    for (const auto& [id, source] : mSignaled)
    {
        const EState id_state = stateForDefaultAnim(id);
        if (id_state == STATE_NONE || id_state == TYPING || !stopsDefaultAnim(id_state))
        {
            continue;
        }
        if (id == ANIM_AGENT_WALK)
        {
            // Every viewer keeps a backwards-walking avatar facing forward only
            // while this built-in is listed (LLVOAvatar::updateOrientation);
            // stopping it turns the avatar round for everybody. (Locally the
            // sync intercept still keeps its motion off our avatar.)
            continue;
        }
        if (keepsStockUnder(id_state))
        {
            continue; // stays under the replacement (sync mode)
        }
        // Still listed although stopped before: the server re-asserted it
        if (mayStopBuiltin(id, requests))
        {
            mStoppedDefaults[id] = id_state;
        }
    }
    // The away slump under an Away replacement
    if (state == AWAY && mSignaled.count(ANIM_AGENT_AWAY) && mayStopBuiltin(ANIM_AGENT_AWAY, requests))
    {
        mStoppedAway = true;
    }
}

void SLAOEngine::restoreSystemAnims(std::vector<std::pair<LLUUID, bool>>& requests)
{
    // Nothing replaces the posture any more (pause, takeover, AO off, no
    // animation): give the built-in back at once so the avatar never ends up
    // without one. Not when the seat animates us: poseballs stop "sit"
    // themselves and would be fought.
    if (!mStoppedDefaults.empty())
    {
        const EState current = mSignaledState;
        const bool sitting = isAgentAvatarValid() && gAgentAvatarp->isSitting();
        for (const auto& [id, id_state] : mStoppedDefaults)
        {
            if (id_state != current || mSignaled.count(id) || mSeatAnimates || (isSitState(id_state) && !sitting))
            {
                continue;
            }
            requests.emplace_back(id, true);
            if (local_playback())
            {
                gAgentAvatarp->startMotion(id);
            }
            break; // one posture built-in is enough
        }
        mStoppedDefaults.clear();
    }
    if (mStoppedAway)
    {
        if (gAgent.getAFK() && !mSignaled.count(ANIM_AGENT_AWAY))
        {
            requests.emplace_back(ANIM_AGENT_AWAY, true);
        }
        mStoppedAway = false;
    }
}

bool SLAOEngine::isLowPriority(const LLUUID& asset_id) const
{
    const LLKeyframeMotion::JointMotionList* data = LLKeyframeDataCache::getKeyframeData(asset_id);
    // Built-ins are mostly priority 0-2: a replacement at or below that only
    // blends with them unless they are stopped
    return data && data->mBasePriority < LLJoint::HIGHER_PRIORITY;
}

void SLAOEngine::sendRequests(const std::vector<std::pair<LLUUID, bool>>& requests)
{
    if (requests.empty() || gAgentID.isNull() || !gAgent.getRegion())
    {
        return;
    }
    // One message for every start and stop of a transition, so the server
    // swaps them in the same update and observers never see a gap.
    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_AgentAnimation);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    for (const auto& [anim, start] : requests)
    {
        msg->nextBlockFast(_PREHASH_AnimationList);
        msg->addUUIDFast(_PREHASH_AnimID, anim);
        msg->addBOOLFast(_PREHASH_StartAnim, start);
    }
    msg->nextBlockFast(_PREHASH_PhysicalAvatarEventList);
    msg->addBinaryDataFast(_PREHASH_TypeData, nullptr, 0);
    gAgent.sendReliableMessage();
}

void SLAOEngine::stopAll(bool restore_defaults)
{
    std::vector<std::pair<LLUUID, bool>> requests;
    for (Channel* channel : { &mMain, &mOverlay })
    {
        if (channel->mAnim.notNull())
        {
            requests.emplace_back(channel->mAnim, false);
            if (local_playback())
            {
                gAgentAvatarp->stopMotion(channel->mAnim);
            }
        }
        *channel = Channel();
    }
    if (restore_defaults)
    {
        restoreSystemAnims(requests);
    }
    mStoppedDefaults.clear();
    sendRequests(requests);
}

//-----------------------------------------------------------------------------
// Periodic work
//-----------------------------------------------------------------------------

void SLAOEngine::onIdle()
{
    const F64 t = now();
    if (t - mLastTick < TICK_SECS)
    {
        return;
    }
    mLastTick = t;
    onTick();
}

void SLAOEngine::onTick()
{
    const F64 t = now();

    updateSeat();

    // Script commands switched off: holds made earlier by scripts no longer count.
    static LLCachedControl<bool> sAcceptScripts(gSavedSettings, "SLAOAcceptScriptCommands", true);
    if (!sAcceptScripts && !mHolds.empty())
    {
        releaseAllHolds();
    }

    // Holds from objects that are gone
    for (auto it = mHolds.begin(); it != mHolds.end(); )
    {
        if (gObjectList.findObject(it->first))
        {
            it->second.mMissingSince = 0.0;
            ++it;
            continue;
        }
        if (it->second.mMissingSince == 0.0)
        {
            it->second.mMissingSince = t;
        }
        if (gAgent.getTeleportState() == LLAgent::TELEPORT_NONE &&
            t - it->second.mMissingSince > HOLD_RELEASE_SECS)
        {
            LL_INFOS() << "AO: releasing hold of vanished object " << it->first << LL_ENDL;
            it = mHolds.erase(it);
            continue;
        }
        ++it;
    }

    for (auto it = mRecentlyStopped.begin(); it != mRecentlyStopped.end(); )
    {
        it = (t - it->second > RECENTLY_STOPPED_SECS) ? mRecentlyStopped.erase(it) : std::next(it);
    }
    for (auto it = mScriptedAOScores.begin(); it != mScriptedAOScores.end(); )
    {
        it = (t - it->second.mFirstHit > SCRIPTED_AO_WINDOW_SECS) ? mScriptedAOScores.erase(it) : std::next(it);
    }

    checkPreloads();
    reconcile();
    reconcileAlways();
    cleanupStrays();
    scanForeign();
    tickIdle();

    // Cycling (stands and any state with cycling on)
    const SLAOSet* set = getCurrentSet();
    if (set && mMain.mState != STATE_NONE && mPauseReason == PAUSE_NONE && mIdleAnim.isNull())
    {
        const SLAOStateConfig& config = set->mStateConfig[mMain.mState];
        const S32 count = (S32)set->mAnims[mMain.mState].size();
        if (config.mCycle && !config.mPickOnEntry && count > 1 && (t - mLastCycleAt) >= (F64)config.mInterval)
        {
            S32& index = mIndex[mMain.mState];
            index = config.mRandom ? (index + 1 + (S32)ll_rand(count - 1)) % count : (index + 1) % count;
            mLastCycleAt = t;
        }
    }

    // Local preview ends with the animation
    if (mPreviewAnim.notNull() && isAgentAvatarValid())
    {
        LLMotion* motion = gAgentAvatarp->findMotion(mPreviewAnim);
        if (!motion || (!gAgentAvatarp->isMotionActive(mPreviewAnim) && LLKeyframeDataCache::getKeyframeData(mPreviewAnim)))
        {
            mPreviewAnim.setNull();
            notifyChanged();
        }
    }

    update();
}

void SLAOEngine::updateSeat()
{
    LLUUID seat;
    if (isAgentAvatarValid() && gAgentAvatarp->isSitting())
    {
        const LLViewerObject* parent = (LLViewerObject*)gAgentAvatarp->getParent();
        const LLViewerObject* root = parent ? parent->getRootEdit() : nullptr;
        if (root)
        {
            seat = root->getID();
        }
    }
    if (seat != mSeatID)
    {
        mSeatID = seat;
        mSatAt = now();
    }
}

void SLAOEngine::reconcile()
{
    const F64 t = now();
    for (Channel* channel : { &mMain, &mOverlay })
    {
        if (channel->mAnim.isNull())
        {
            continue;
        }
        if (mSignaled.count(channel->mAnim))
        {
            channel->mConfirmed = true;
            continue;
        }
        if (channel->mConfirmed)
        {
            // It was playing and something stopped it
            if (mBodyTakeover)
            {
                // A script cleared our animations to play its own (poseballs
                // and some collars stop everything): do not fight it; it is
                // picked again when the script animation ends.
                if (local_playback())
                {
                    gAgentAvatarp->stopMotion(channel->mAnim);
                }
                channel->mAnim.setNull();
                channel->mState = STATE_NONE;
                continue;
            }
            channel->mConfirmed = false;
            channel->mSentAt = t - CONFIRM_TIMEOUT_SECS; // retry now
        }
        if (t - channel->mSentAt < CONFIRM_TIMEOUT_SECS)
        {
            continue;
        }
        if (channel->mRetries >= MAX_RETRIES)
        {
            continue; // keep it locally; the next state change starts afresh
        }
        // Lost (teleport, region crossing, dropped request): ask again
        ++channel->mRetries;
        channel->mSentAt = t;
        sendRequests({ { channel->mAnim, true } });
    }
}

//-----------------------------------------------------------------------------
// Always: body-fix animations that run as long as the AO is on
//-----------------------------------------------------------------------------

void SLAOEngine::updateAlways()
{
    std::set<LLUUID> desired;
    const SLAOSet* set = getCurrentSet();
    bool hold_all = false;
    for (const auto& [id, hold] : mHolds)
    {
        hold_all = hold_all || (hold.mMask & HOLD_ALL);
    }
    // Fixups, not motion: only switching the AO (or Always) off, or a full
    // @slao=n hold, stops them; takeovers, sits and typing do not.
    if (isEnabled() && set && set->mAlwaysEnabled && !hold_all && isAgentAvatarValid() && gAgent.getRegion())
    {
        for (const SLAOAnim& anim : set->mAnims[ALWAYS])
        {
            if (anim.mAssetID.notNull() && !mBadAnims.count(anim.mAssetID) && desired.size() < MAX_ALWAYS)
            {
                desired.insert(anim.mAssetID);
            }
        }
    }

    std::vector<std::pair<LLUUID, bool>> requests;
    const F64 t = now();
    for (auto it = mAlways.begin(); it != mAlways.end(); )
    {
        if (desired.count(it->first))
        {
            ++it;
            continue;
        }
        requests.emplace_back(it->first, false);
        if (local_playback())
        {
            gAgentAvatarp->stopMotion(it->first);
        }
        mRecentlyStopped[it->first] = t;
        noteOwnStop(it->first);
        it = mAlways.erase(it);
    }
    for (const LLUUID& anim : desired)
    {
        if (mAlways.count(anim))
        {
            continue;
        }
        AlwaysEntry entry;
        entry.mSentAt = t;
        entry.mConfirmed = mSignaled.count(anim) != 0;
        mAlways[anim] = entry;
        mOwnStopped.erase(anim);
        requests.emplace_back(anim, true);
        if (local_playback())
        {
            gAgentAvatarp->startMotion(anim);
        }
    }
    sendRequests(requests);
}

void SLAOEngine::updateHeadHold()
{
    // LLHeadRotMotion (medium priority) turns the head, neck and torso toward
    // the look-at point on top of every animation that leaves those joints
    // alone. Other viewers do the same to our avatar from the look-at target
    // we send. While a replacement plays, hold the head: stop the motion on
    // our avatar and send no look-at target (LLAgentCamera::setLookAt asks
    // holdsHead()). Eye motion is left alone: it moves only the eyes.
    static LLCachedControl<bool> sKeepHead(gSavedSettings, "SLAOKeepHeadStill", true);
    const bool hold = sKeepHead && isEnabled() && mPauseReason == PAUSE_NONE && mMain.mAnim.notNull() &&
                      isAgentAvatarValid();
    if (hold == mHoldingHead)
    {
        if (hold && gAgentAvatarp->isMotionActive(ANIM_AGENT_HEAD_ROT))
        {
            gAgentAvatarp->stopMotion(ANIM_AGENT_HEAD_ROT);   // something restarted it
        }
        return;
    }
    mHoldingHead = hold;
    if (!isAgentAvatarValid())
    {
        return;
    }
    if (hold)
    {
        gAgentAvatarp->stopMotion(ANIM_AGENT_HEAD_ROT);
        gAgentAvatarp->removeAnimationData(LLCharacter::ANIM_DATA_LOOK_AT_POINT);
        if (gAgentCamera.mLookAt.notNull() && !gAgentCamera.mLookAt->isDead())
        {
            // Tell other viewers there is nothing to look at
            gAgentCamera.mLookAt->clearLookAtTarget();
            gAgentCamera.mLookAt->setNeedsSendToSim(true);
        }
    }
    else
    {
        // Normal head motion again (as LLVOAvatar starts it); look-at targets
        // are sent again from the next one the camera sets
        gAgentAvatarp->startMotion(ANIM_AGENT_HEAD_ROT);
    }
}

void SLAOEngine::reconcileAlways()
{
    // Same recheck as the posture channel: missing from the server's list for
    // 2 s (teleport, crossing, relog, a script stopping everything) -> again
    const F64 t = now();
    std::vector<std::pair<LLUUID, bool>> requests;
    for (auto& [anim, entry] : mAlways)
    {
        if (mSignaled.count(anim))
        {
            entry.mConfirmed = true;
            entry.mRetries = 0;
            continue;
        }
        if (entry.mConfirmed)
        {
            entry.mConfirmed = false;
            entry.mSentAt = t - CONFIRM_TIMEOUT_SECS;
        }
        if (t - entry.mSentAt < CONFIRM_TIMEOUT_SECS || entry.mRetries >= MAX_RETRIES)
        {
            continue;
        }
        ++entry.mRetries;
        entry.mSentAt = t;
        requests.emplace_back(anim, true);
    }
    sendRequests(requests);
}

//-----------------------------------------------------------------------------
// Idle: a random animation now and then in place of the stand
//-----------------------------------------------------------------------------

bool SLAOEngine::isIdleHost(EState state) const
{
    const SLAOSet* set = getCurrentSet();
    if (!set)
    {
        return false;
    }
    return state == STAND || (set->mIdleWhileSitting && isSitState(state));
}

bool SLAOEngine::idleAllowed() const
{
    const SLAOSet* set = getCurrentSet();
    if (!set || !set->mIdleEnabled || !set->hasAnims(IDLE) || mPauseReason != PAUSE_NONE ||
        !isIdleHost(mMain.mState) || mMain.mAnim.isNull())
    {
        return false;
    }
    if (mLastControlMask & MOVE_MASK)
    {
        return false; // moving
    }
    if (gAgentCamera.cameraMouselook())
    {
        return false;
    }
    if (mTypingSignaled && set->mNoStandsWhileTyping)
    {
        return false;
    }
    // Takeovers and @slao:stand holds (IDLE is an idle state)
    return pauseFor(IDLE) == PAUSE_NONE;
}

void SLAOEngine::scheduleIdle()
{
    const SLAOSet* set = getCurrentSet();
    if (!set)
    {
        mNextIdleAt = 0.0;
        return;
    }
    const F32 span = llmax(0.f, set->mIdleMaxDelay - set->mIdleMinDelay);
    mNextIdleAt = now() + (F64)(set->mIdleMinDelay + ll_frand(span));
}

void SLAOEngine::startIdle()
{
    const SLAOSet* set = getCurrentSet();
    if (!set)
    {
        return;
    }
    // Which idle: the Idle state's Cycle / Random options, each idle being
    // one entry. Cycle off: the first; Cycle on: the next in order, or a
    // random one never repeating with Random. Failed animations are skipped.
    const std::vector<SLAOAnim>& idles = set->mAnims[IDLE];
    const S32 count = (S32)idles.size();
    const SLAOStateConfig& config = set->mStateConfig[IDLE];
    S32& index = mIndex[IDLE];
    if (!config.mCycle || count < 2)
    {
        index = 0;
    }
    else if (config.mRandom)
    {
        index = mEverPicked[IDLE] ? (index + 1 + (S32)ll_rand(count - 1)) % count : (S32)ll_rand(count);
    }
    else if (mEverPicked[IDLE])
    {
        index = (index + 1) % count;
    }
    mEverPicked[IDLE] = true;
    const SLAOAnim* pick = nullptr;
    for (S32 k = 0; k < count && !pick; ++k)
    {
        const SLAOAnim& candidate = idles[(index + k) % count];
        if (candidate.mAssetID.notNull() && !mBadAnims.count(candidate.mAssetID))
        {
            pick = &candidate;
            index = (index + k) % count;
        }
    }
    if (!pick)
    {
        scheduleIdle();
        return;
    }

    // Its own length, unless it loops or runs long
    F32 duration = set->mIdleMaxDuration;
    if (const LLKeyframeMotion::JointMotionList* data = LLKeyframeDataCache::getKeyframeData(pick->mAssetID))
    {
        if (!data->mLoop && data->mDuration > 0.f)
        {
            duration = llmin(data->mDuration, set->mIdleMaxDuration);
        }
    }
    mIdleAnim = pick->mAssetID;
    mLastIdle = mIdleAnim;
    mIdleUntil = now() + (F64)llmax(duration, 1.f);
    update();   // swaps the stand for the idle in one message
}

void SLAOEngine::endIdle(bool reschedule)
{
    if (mIdleAnim.isNull())
    {
        return;
    }
    mIdleAnim.setNull();
    mLastCycleAt = now();
    if (reschedule)
    {
        scheduleIdle();
    }
    else
    {
        mNextIdleAt = 0.0;
    }
}

void SLAOEngine::tickIdle()
{
    const F64 t = now();
    if (mIdleAnim.notNull())
    {
        if (t >= mIdleUntil || !idleAllowed())
        {
            endIdle(true);
            update(); // back to the stand in one message
        }
        return;
    }
    if (!idleAllowed())
    {
        // Count again from the next time the stand is undisturbed
        mNextIdleAt = 0.0;
        return;
    }
    if (mNextIdleAt == 0.0)
    {
        scheduleIdle();
    }
    else if (t >= mNextIdleAt)
    {
        startIdle();
    }
}

void SLAOEngine::onRegionChanged()
{
    mYawTime = 0.0; // a teleport is not a turn
    for (auto& [anim, entry] : mAlways)
    {
        entry.mConfirmed = false;
        entry.mSentAt = now();
        entry.mRetries = 0;
    }
    // The new region rebuilds the animation list; make sure ours survive.
    for (Channel* channel : { &mMain, &mOverlay })
    {
        channel->mConfirmed = false;
        channel->mSentAt = now();
        channel->mRetries = 0;
    }
    mDefaultStopTimes.clear();
    mOwnStopped.clear();    // the old region's list is gone
    resendOnArrival();
}

//-----------------------------------------------------------------------------
// Seamless state changes (#668, setting SLAOSyncIntercept)
//
// The server starts the built-in state animation (stand, walk, fly, ...) and
// the viewer plays it on our avatar as soon as the AvatarAnimation message
// arrives. In sync mode that message reaches the AO first
// (process_avatar_animation): the new state's replacement is picked, started
// locally and requested from the server; then, as the avatar starts the
// built-ins of the list, LLVOAvatar::startMotion asks interceptStockStart()
// and skips every built-in the replacement covers. No default frame shows on
// our avatar and nothing has to be chased afterwards. Stopping the built-ins
// on the server (for observers) is unchanged: stopSystemAnims().
//
// Guards:
//  - syncLocalStock() runs after every decision: built-ins kept off start
//    again at once when the replacement goes (AO off, pause, takeover, empty
//    state), and built-ins already playing when the AO takes over mid-state
//    (switched on, resumed, set reloaded, replacement just loaded) ease out
//    under it. Decisions follow the server's whole list, not single start
//    and stop events, so the stop that follows enabling or reloading
//    mid-state cannot end our replacement.
//  - The pre-jump, landings and stand-up keep their built-in with the
//    replacement layered on top (the viewer ends those states when the motion
//    ends); the sit keeps it as a base pose under the sit replacement (unless
//    that is low priority and would only blend); the ground sit and typing
//    are never touched. Seat animations still pause the sit replacement.
//  - Flight switched by the viewer itself (simulated swimming) predicts the
//    new air or ground state at once, so the region's echo of the change
//    matches what already plays instead of swapping it back and forth.
//  - Replacements we stopped that the server still lists are stopped again.
//  - Region arrival re-sends the current replacements immediately.
// With the setting off the AO works as before (after the fact).
//-----------------------------------------------------------------------------

bool SLAOEngine::syncInterceptEnabled() const
{
    static LLCachedControl<bool> sSync(gSavedSettings, "SLAOSyncIntercept", true);
    return sSync;
}

bool SLAOEngine::wantsAnimationsFirst() const
{
    return mInitialized && syncInterceptEnabled();
}

bool SLAOEngine::keepsStockUnder(EState stock_state) const
{
    if (!syncInterceptEnabled())
    {
        return false;
    }
    if (is_transient(stock_state))
    {
        return true;
    }
    // Base pose under the sit replacement; a low-priority replacement would
    // only blend with it, so then it is stopped as before
    return stock_state == SIT && !isLowPriority(mMain.mAnim);
}

bool SLAOEngine::suppressesStockMotion(const LLUUID& anim_id) const
{
    if (!mInitialized || !syncInterceptEnabled())
    {
        return false;
    }
    const EState stock = stateForDefaultAnim(anim_id);
    if (stock == STATE_NONE || keeps_stock_locally(stock))
    {
        return false;
    }
    // Only while a replacement plays on the posture, and only where it stops
    // the built-ins anyway (the set can keep them: "Stop system animations")
    if (!isEnabled() || mPauseReason != PAUSE_NONE || mMain.mAnim.isNull() || !local_playback() ||
        !stopSystemAnimsEnabled() || !stopsDefaultAnim(mMain.mState))
    {
        return false;
    }
    // Not before the replacement can play: until then the built-in holds the pose
    return LLKeyframeDataCache::getKeyframeData(mMain.mAnim) != nullptr;
}

bool SLAOEngine::interceptStockStart(const LLUUID& anim_id)
{
    if (mInUpdate || !suppressesStockMotion(anim_id))
    {
        return false;
    }
    if (mSuppressed.insert(anim_id).second)
    {
        LL_DEBUGS("SLAO") << "built-in " << gAnimLibrary.animationName(anim_id) << " kept off under "
                          << stateLabel(mMain.mState) << LL_ENDL;
    }
    return true;
}

void SLAOEngine::onStockStop(const LLUUID& anim_id)
{
    if (!mInUpdate)
    {
        mSuppressed.erase(anim_id);
    }
}

void SLAOEngine::syncLocalStock()
{
    if (!isAgentAvatarValid())
    {
        mSuppressed.clear();
        return;
    }
    LLVOAvatarSelf* avatar = gAgentAvatarp;

    // Kept off, and the replacement is gone: the built-in plays again at once
    // (startMotion is a no-op on a running motion and restarts one easing out)
    for (auto it = mSuppressed.begin(); it != mSuppressed.end(); )
    {
        const LLUUID id = *it;
        const bool signaled = avatar->mSignaledAnimations.count(id) != 0;
        const bool playing = avatar->mPlayingAnimations.count(id) != 0;
        if (!signaled && !playing)
        {
            it = mSuppressed.erase(it); // gone from both: nothing to give back
            continue;
        }
        if (suppressesStockMotion(id))
        {
            ++it;
            continue;
        }
        it = mSuppressed.erase(it);
        if (signaled && playing)
        {
            LL_DEBUGS("SLAO") << "built-in " << gAnimLibrary.animationName(id) << " back on" << LL_ENDL;
            avatar->startMotion(id);
        }
    }

    if (!syncInterceptEnabled() || mMain.mAnim.isNull() || mPauseReason != PAUSE_NONE)
    {
        return;
    }
    // Already playing when the replacement took over: ease out under it
    for (const auto& [id, sequence] : avatar->mSignaledAnimations)
    {
        if (mSuppressed.count(id) || !suppressesStockMotion(id))
        {
            continue;
        }
        mSuppressed.insert(id);
        if (avatar->isMotionActive(avatar->remapMotionID(id)))
        {
            LL_DEBUGS("SLAO") << "built-in " << gAnimLibrary.animationName(id) << " eased out under "
                              << stateLabel(mMain.mState) << LL_ENDL;
            avatar->stopMotion(id);
        }
    }
}

void SLAOEngine::noteOwnTransition()
{
    mOwnTransitionAt = now();
    LL_DEBUGS("SLAO") << "own posture transition, flight " << (gAgent.getFlying() ? "on" : "off") << LL_ENDL;
}

void SLAOEngine::noteOwnStop(const LLUUID& anim_id)
{
    if (anim_id.isNull())
    {
        return;
    }
    if (mOwnStopped.size() >= MAX_OWN_STOPPED && !mOwnStopped.count(anim_id))
    {
        mOwnStopped.clear();
    }
    OwnStop& entry = mOwnStopped[anim_id];
    entry.mSentAt = now();
    entry.mRetries = 0;
}

void SLAOEngine::cleanupStrays()
{
    // A replacement of an earlier state (or an Always animation switched off)
    // that the server still lists after our stop: the stop was lost, or a
    // retry of the old start arrived after it. Stop it again, a few times.
    if (!syncInterceptEnabled())
    {
        mOwnStopped.clear();
        return;
    }
    const F64 t = now();
    std::vector<std::pair<LLUUID, bool>> requests;
    for (auto it = mOwnStopped.begin(); it != mOwnStopped.end(); )
    {
        const LLUUID anim = it->first;
        const auto signaled = mSignaled.find(anim);
        const bool listed_as_ours = signaled != mSignaled.end() &&
                                    (signaled->second.isNull() || signaled->second == gAgentID);
        const bool wanted = anim == mMain.mAnim || anim == mOverlay.mAnim || anim == mPreviewAnim ||
                            mAlways.count(anim) != 0;
        if (wanted || !listed_as_ours || it->second.mRetries >= MAX_RETRIES)
        {
            it = mOwnStopped.erase(it);
            continue;
        }
        if (t - it->second.mSentAt >= CONFIRM_TIMEOUT_SECS)
        {
            ++it->second.mRetries;
            it->second.mSentAt = t;
            mRecentlyStopped[anim] = t;
            requests.emplace_back(anim, false);
            if (local_playback())
            {
                gAgentAvatarp->stopMotion(anim);
            }
            LL_DEBUGS("SLAO") << "stray replacement " << anim << " still playing; stopping it again" << LL_ENDL;
        }
        ++it;
    }
    sendRequests(requests);
}

void SLAOEngine::resendOnArrival()
{
    // The new region starts from its own animation list: ask for ours now
    // rather than after the confirmation timeout (which stays as a backstop)
    if (!mInitialized || !syncInterceptEnabled() || !isEnabled() || !isAgentAvatarValid() || !gAgent.getRegion())
    {
        return;
    }
    std::vector<std::pair<LLUUID, bool>> requests;
    if (mMain.mAnim.notNull() && mPauseReason == PAUSE_NONE &&
        (!isSitState(mMain.mState) || gAgentAvatarp->isSitting()))
    {
        requests.emplace_back(mMain.mAnim, true);
    }
    if (mOverlay.mAnim.notNull())
    {
        requests.emplace_back(mOverlay.mAnim, true);
    }
    for (const auto& [anim, entry] : mAlways)
    {
        requests.emplace_back(anim, true);
    }
    if (!requests.empty())
    {
        LL_DEBUGS("SLAO") << "region arrival: re-sending " << requests.size() << " animation(s), state "
                          << stateLabel(mMain.mState) << LL_ENDL;
        sendRequests(requests);
    }
}

//-----------------------------------------------------------------------------
// Foreign animations
//-----------------------------------------------------------------------------

SLAOEngine::EClass SLAOEngine::classify(const LLUUID& anim_id)
{
    auto it = mClassCache.find(anim_id);
    if (it != mClassCache.end())
    {
        return it->second.mClass;
    }
    const LLKeyframeMotion::JointMotionList* joints = LLKeyframeDataCache::getKeyframeData(anim_id);
    if (!joints)
    {
        return CLS_PENDING; // not loaded yet; asked again next tick
    }

    // A pose rotates the pelvis, torso or legs away from the rest pose
    // (animated over time, or held still). Touching those joints with
    // position keys only (deformers, bone offsets) or with rest rotations
    // only is a body fix, which plays fine under the AO.
    ClassInfo info;
    info.mClass = CLS_PARTIAL;
    bool touches_body = false;
    for (U32 i = 0; i < joints->getNumJointMotions(); ++i)
    {
        const LLKeyframeMotion::JointMotion* joint = joints->getJointMotion(i);
        if (!joint || !is_body_joint(joint->mJointName))
        {
            continue;
        }
        if (joint->mRotationCurve.mNumKeys > 0 || joint->mPositionCurve.mNumKeys > 0)
        {
            touches_body = true;
        }
        bool poses = false;
        for (const auto& [time, key] : joint->mRotationCurve.mKeys)
        {
            // Angle away from the identity (= rest) rotation
            const F32 w = llclamp(fabsf(key.mRotation.mQ[VW]), 0.f, 1.f);
            if (2.f * acosf(w) > POSE_MIN_ANGLE_RAD)
            {
                poses = true;
                break;
            }
        }
        if (poses)
        {
            info.mClass = CLS_BODY;
            const S32 priority = (joint->mPriority == LLJoint::USE_MOTION_PRIORITY)
                                 ? (S32)joints->mBasePriority : (S32)joint->mPriority;
            info.mPosePriority = llmax(info.mPosePriority, priority);
        }
    }
    if (info.mClass != CLS_BODY && touches_body)
    {
        info.mClass = CLS_BODY_FIX;
    }
    if (mClassCache.size() >= MAX_CLASS_CACHE)
    {
        mClassCache.clear();
    }
    mClassCache[anim_id] = info;
    return info.mClass;
}

S32 SLAOEngine::posePriority(const LLUUID& anim_id)
{
    if (classify(anim_id) == CLS_PENDING)
    {
        return -1;
    }
    return mClassCache[anim_id].mPosePriority;
}

S32 SLAOEngine::ownPriority() const
{
    if (mMain.mAnim.notNull())
    {
        if (const LLKeyframeMotion::JointMotionList* data = LLKeyframeDataCache::getKeyframeData(mMain.mAnim))
        {
            return (S32)data->mBasePriority;
        }
    }
    // Paused: compare with what played last, so the decision does not flip
    return mLastOwnPriority;
}

bool SLAOEngine::isAlwaysAssetAnywhere(const LLUUID& asset_id) const
{
    // The user's own Always/Deformer animations, also when an attachment plays them
    for (const auto& [id, set] : SLAOSets::instance().getSets())
    {
        for (const SLAOAnim& anim : set.mAnims[ALWAYS])
        {
            if (anim.mAssetID == asset_id)
            {
                return true;
            }
        }
    }
    return false;
}

std::string SLAOEngine::getTakeoverSourceName() const
{
    std::string name = attachment_name(gObjectList.findObject(mTakeoverSource));
    return name.empty() ? LLTrans::getString("SLAOUnknownScript") : name;
}

void SLAOEngine::ignoreTakeoverAnim()
{
    if (mTakeoverAnim.isNull())
    {
        return;
    }
    LL_INFOS() << "AO: user chose not to pause for animation " << mTakeoverAnim << " from '"
               << getTakeoverSourceName() << "'" << LL_ENDL;
    SLAOSets::instance().addIgnoredAnim(mTakeoverAnim, getTakeoverSourceName());
    scanForeign();
    update();
    notifyChanged();
}

void SLAOEngine::scanForeign()
{
    S32 body_fixes = 0;
    if (mMain.mAnim.notNull())
    {
        if (const LLKeyframeMotion::JointMotionList* data = LLKeyframeDataCache::getKeyframeData(mMain.mAnim))
        {
            mLastOwnPriority = (S32)data->mBasePriority;
        }
    }
    mBodyTakeover = false;
    mSeatAnimates = false;
    mServerOverride = false;
    mTakeoverSource.setNull();
    mTakeoverAnim.setNull();

    bool has_default_state = false;
    for (const auto& [anim, source] : mSignaled)
    {
        const EState state = stateForDefaultAnim(anim);
        if (state != STATE_NONE && state != TYPING)
        {
            has_default_state = true;
            break;
        }
    }

    for (const auto& [anim, source] : mSignaled)
    {
        if (isBuiltinAnim(anim) || anim == mMain.mAnim || anim == mOverlay.mAnim || anim == mPreviewAnim)
        {
            continue;
        }
        const bool agent_source = source.isNull() || source == gAgentID;
        if (agent_source && (mRecentlyStopped.count(anim) || isSetAnim(anim)))
        {
            continue; // ours, on its way out
        }
        const LLViewerObject* object = agent_source ? nullptr : gObjectList.findObject(source);
        const LLViewerObject* root = object ? object->getRootEdit() : nullptr;
        if (mSeatID.notNull() && root && root->getID() == mSeatID)
        {
            mSeatAnimates = true;
        }
        const EClass cls = classify(anim);
        const bool ignored = SLAOSets::instance().isIgnoredAnim(anim) || mAlways.count(anim) ||
                             isAlwaysAssetAnywhere(anim);
        if (cls == CLS_BODY_FIX || (ignored && cls != CLS_PARTIAL))
        {
            ++body_fixes;
            if (!mLoggedBodyFixes.count(anim) && mLoggedBodyFixes.size() < 256)
            {
                mLoggedBodyFixes.insert(anim);
                LL_INFOS() << "AO: ignoring " << (ignored ? "listed" : "deformer/body-fix") << " animation " << anim
                           << " from '" << (agent_source ? std::string("you") : attachment_name(object)) << "'" << LL_ENDL;
            }
            continue;
        }
        if (cls == CLS_BODY)
        {
            // A pose on the body joints below our replacement's priority loses
            // to it anyway: no need to pause (a priority-4 collar pose does pause)
            const S32 theirs = posePriority(anim);
            const S32 ours = ownPriority();
            if (theirs >= 0 && ours >= 0 && theirs < ours)
            {
                continue;
            }
            if (!mBodyTakeover)
            {
                mTakeoverSource = source;
                mTakeoverAnim = anim;
            }
            mBodyTakeover = true;
            if (agent_source && !has_default_state)
            {
                // No built-in state animation and a body animation the agent
                // "started": a script replaces states server-side
                // (llSetAnimationOverride).
                mServerOverride = true;
            }
        }
    }

    mBodyFixCount = body_fixes;
    if (mLoggedBodyFixes.size() >= 256)
    {
        mLoggedBodyFixes.clear();
    }

    if (mSeatAnimates && mSeatID.notNull() && !mAnimatingSeats.count(mSeatID))
    {
        if (mAnimatingSeats.size() >= MAX_SEATS)
        {
            mAnimatingSeats.clear();
        }
        mAnimatingSeats.insert(mSeatID);
    }
}

void SLAOEngine::noteScriptedAOActivity(const LLUUID& anim_id, const LLUUID& source_id)
{
    static LLCachedControl<bool> sDetect(gSavedSettings, "SLAODetectScriptedAO", true);
    if (!sDetect || !isEnabled() || isBuiltinAnim(anim_id) || isSetAnim(anim_id) || mAlways.count(anim_id) ||
        classify(anim_id) == CLS_BODY_FIX || SLAOSets::instance().isIgnoredAnim(anim_id) ||
        anim_id == mMain.mAnim || anim_id == mOverlay.mAnim || mRecentlyStopped.count(anim_id))
    {
        return;
    }
    const F64 t = now();
    // A scripted AO reacts to movement: its animation shows up right after a
    // state change or a movement key.
    const F64 trigger = llmax(mStateChangedAt, mControlsChangedAt);
    if (t - trigger > SCRIPTED_AO_REACTION_SECS || isSitState(mSignaledState))
    {
        return;
    }

    LLUUID key;          // attachment inventory item, or null for server-side
    std::string name;
    const bool agent_source = source_id.isNull() || source_id == gAgentID;
    if (!agent_source)
    {
        const LLViewerObject* object = gObjectList.findObject(source_id);
        const LLViewerObject* root = object ? object->getRootEdit() : nullptr;
        if (!root || !root->isAttachment() || !root->permYouOwner())
        {
            return; // poseballs, furniture, other people's things
        }
        key = root->getAttachmentItemID();
        name = attachment_name(root);
        if (key.isNull() || SLAOSets::instance().getScriptedAOChoice(key) == "ignore")
        {
            return;
        }
    }
    if (mScriptedAOOffered.count(key))
    {
        return;
    }

    if (!mScriptedAOScores.count(key) && mScriptedAOScores.size() >= MAX_SCRIPTED_AO_SCORES)
    {
        return;
    }
    ScriptedAOScore& score = mScriptedAOScores[key];
    if (score.mHits == 0)
    {
        score.mFirstHit = t;
    }
    ++score.mHits;
    if (score.mAnims.size() < 16)
    {
        score.mAnims.insert(anim_id);
    }
    score.mName = name;
    if (score.mHits >= SCRIPTED_AO_HITS && score.mAnims.size() >= SCRIPTED_AO_MIN_ANIMS)
    {
        mScriptedAOOffered.insert(key);
        mScriptedAOScores.erase(key);
        offerScriptedAOPause(key, name);
    }
}

void SLAOEngine::offerScriptedAOPause(const LLUUID& item_id, const std::string& name)
{
    LLSD args;
    args["NAME"] = name.empty() ? LLTrans::getString("SLAOUnknownScript") : name;
    LLSD payload;
    payload["item_id"] = item_id;
    LL_INFOS() << "AO: scripted AO activity detected from '" << name << "' (" << item_id << ")" << LL_ENDL;
    LLNotificationsUtil::add(item_id.notNull() ? "SLAOScriptedAODetected" : "SLAOServerAODetected", args, payload,
        [](const LLSD& notification, const LLSD& response)
        {
            if (!SLAOEngine::instanceExists())
            {
                return;
            }
            const S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
            const LLUUID id = notification["payload"]["item_id"].asUUID();
            SLAOEngine& self = SLAOEngine::instance();
            switch (option)
            {
            case 0: // defer to it while worn / pause for this session
                if (id.notNull())
                {
                    SLAOSets::instance().setScriptedAOChoice(id, "pause");
                    self.update();
                    self.notifyChanged();
                }
                else
                {
                    self.setUserPaused(true);
                }
                break;
            case 1: // turn ours off
                self.setEnabled(false);
                break;
            case 3: // take its animations into a set of ours
            {
                LLViewerObject* hud = (id.notNull() && isAgentAvatarValid()) ? gAgentAvatarp->getWornAttachment(id) : nullptr;
                LLFloaterReg::showInstance("sl_ao_import_object", hud ? LLSD(hud->getID()) : LLSD());
                break;
            }
            default: // keep both (and do not ask about this one again)
                if (id.notNull())
                {
                    SLAOSets::instance().setScriptedAOChoice(id, "ignore");
                }
                break;
            }
        });
}

bool SLAOEngine::isSetAnim(const LLUUID& asset_id) const
{
    const SLAOSet* set = getCurrentSet();
    if (!set)
    {
        return false;
    }
    for (const auto& anims : set->mAnims)
    {
        for (const SLAOAnim& anim : anims)
        {
            if (anim.mAssetID == asset_id)
            {
                return true;
            }
        }
    }
    return false;
}

void SLAOEngine::resumeFromScriptedAO()
{
    // "Keep both" for every worn scripted AO we were deferring to
    std::vector<LLUUID> ids;
    if (isAgentAvatarValid())
    {
        for (const auto& [item_id, choice] : SLAOSets::instance().getScriptedAOChoices())
        {
            if (choice == "pause" && gAgentAvatarp->isWearingAttachment(item_id))
            {
                ids.push_back(item_id);
            }
        }
    }
    for (const LLUUID& id : ids)
    {
        SLAOSets::instance().setScriptedAOChoice(id, "ignore");
    }
    LL_INFOS() << "AO: resumed from scripted AO deferral (" << ids.size() << " item(s))" << LL_ENDL;
    update();
    notifyChanged();
}

bool SLAOEngine::isScriptedAOWorn() const
{
    static LLCachedControl<bool> sDetect(gSavedSettings, "SLAODetectScriptedAO", true);
    if (!sDetect || !isAgentAvatarValid())
    {
        return false;
    }
    for (const auto& [item_id, choice] : SLAOSets::instance().getScriptedAOChoices())
    {
        if (choice == "pause" && gAgentAvatarp->isWearingAttachment(item_id))
        {
            return true;
        }
    }
    return false;
}

//-----------------------------------------------------------------------------
// Preloading and failed animations
//-----------------------------------------------------------------------------

void SLAOEngine::preloadSet()
{
    const SLAOSet* set = getCurrentSet();
    if (!set || !gAssetStorage)
    {
        return;
    }
    // Fetch the set's animations into the local cache (no motion is created,
    // so the avatar's motion controller is not flooded); the reply also tells
    // us which animations do not exist.
    for (const auto& anims : set->mAnims)
    {
        for (const SLAOAnim& anim : anims)
        {
            if (mFetchRequested.size() >= MAX_FETCH_REQUESTS)
            {
                return;
            }
            const LLUUID asset_id = anim.mAssetID;
            if (asset_id.isNull() || mBadAnims.count(asset_id) || mFetchRequested.count(asset_id) ||
                LLKeyframeDataCache::getKeyframeData(asset_id))
            {
                continue;
            }
            mFetchRequested.insert(asset_id);
            LLGetAssetCallback callback = [](const LLUUID& id, LLAssetType::EType, void*, S32 status, LLExtStat)
            {
                if (SLAOEngine::instanceExists())
                {
                    SLAOEngine::instance().onAssetFetched(id, status);
                }
            };
            gAssetStorage->getAssetData(asset_id, LLAssetType::AT_ANIMATION, callback, nullptr, false);
        }
    }
}

void SLAOEngine::onAssetFetched(const LLUUID& asset_id, S32 status)
{
    // Only answers that mean "this will never load" mark it bad; network
    // trouble is retried naturally the next time it plays.
    if (status == LL_ERR_ASSET_REQUEST_NOT_IN_DATABASE || status == LL_ERR_INSUFFICIENT_PERMISSIONS)
    {
        if (mBadAnims.size() >= MAX_BAD)
        {
            mBadAnims.clear();
        }
        mBadAnims.insert(asset_id);
        LL_WARNS() << "AO animation " << asset_id << " is not available (" << status << "); skipping it" << LL_ENDL;
        update();
        notifyChanged();
    }
}

void SLAOEngine::checkPreloads()
{
    const F64 t = now();
    bool changed = false;
    for (auto it = mPreloading.begin(); it != mPreloading.end(); )
    {
        if (LLKeyframeDataCache::getKeyframeData(it->first))
        {
            it = mPreloading.erase(it);
            continue;
        }
        if (t - it->second > PRELOAD_TIMEOUT_SECS)
        {
            // Played but never loaded, and its motion is gone: it failed
            // (keyframe data is only evicted for local/preview animations)
            if (isAgentAvatarValid() && !gAgentAvatarp->findMotion(it->first))
            {
                if (mBadAnims.size() >= MAX_BAD)
                {
                    mBadAnims.clear();
                }
                mBadAnims.insert(it->first);
                LL_WARNS() << "AO animation " << it->first << " failed to load; skipping it" << LL_ENDL;
                changed = true;
            }
            it = mPreloading.erase(it);
            continue;
        }
        ++it;
    }
    if (changed)
    {
        notifyChanged();
    }
}

//-----------------------------------------------------------------------------
// Preview
//-----------------------------------------------------------------------------

void SLAOEngine::previewAnimation(const LLUUID& asset_id)
{
    stopPreview();
    if (asset_id.isNull() || !isAgentAvatarValid())
    {
        return;
    }
    mPreviewAnim = asset_id;
    gAgentAvatarp->startMotion(asset_id);
    notifyChanged();
}

void SLAOEngine::stopPreview()
{
    if (mPreviewAnim.notNull() && isAgentAvatarValid())
    {
        gAgentAvatarp->stopMotion(mPreviewAnim);
    }
    mPreviewAnim.setNull();
    notifyChanged();
}

//-----------------------------------------------------------------------------
// Commands from the agent's own objects (llOwnerSay)
//
//   @slao=n / @slao=y            hold / release: pause the whole AO
//   @slao:stand=n / =y           hold / release: pause stands and other idle
//                                states only (ZHAO_STANDOFF / ZHAO_STANDON)
//   @slao:sit=n / =y             hold / release: pause sit overrides only
//   @slao:on=force, @slao:off=force, @slao:next=force, @slao:set:<name>=force
//   @slao=<channel>, @slao:status=<channel>
//                                reply "slao,<version>,<on|off>,<active|paused>,<set>"
//
// Holds are per object, released by "=y", by RLV's @clear from the same
// object (@clear=<filter>: only holds whose name contains the filter), when the
// object is gone, by "/ao resume", or when script commands are switched off.
// Status replies are budgeted (10 per 10 s). The "@" prefix keeps the lines silent
// on other RLV viewers; the commands work with RLV off too.
//-----------------------------------------------------------------------------

bool SLAOEngine::processOwnerSay(const LLUUID& object_id, const std::string& object_name, std::string& message)
{
    static LLCachedControl<bool> sAccept(gSavedSettings, "SLAOAcceptScriptCommands", true);
    if (!mInitialized || !sAccept || message.size() < 5 || message[0] != '@')
    {
        return false;
    }
    std::string lower = message;
    LLStringUtil::toLower(lower);
    if (lower.find("slao") == std::string::npos && lower.find("clear") == std::string::npos)
    {
        return false;
    }

    static LLCachedControl<bool> sCanToggle(gSavedSettings, "SLAOScriptsCanToggle", true);

    bool handled = false;
    std::string kept;
    std::string::size_type start = 1;
    while (start <= message.size())
    {
        std::string::size_type end = message.find(',', start);
        if (end == std::string::npos)
        {
            end = message.size();
        }
        std::string token = message.substr(start, end - start);
        start = end + 1;
        LLStringUtil::trim(token);
        if (token.empty())
        {
            continue;
        }
        std::string cmd = token;
        LLStringUtil::toLower(cmd);

        if (cmd.compare(0, 4, "slao") != 0)
        {
            if (cmd == "clear")
            {
                releaseHolds(object_id);
            }
            else if (cmd.compare(0, 6, "clear=") == 0)
            {
                // RLV semantics: only restrictions whose name contains the filter.
                releaseHolds(object_id, cmd.substr(6));
            }
            kept += (kept.empty() ? "" : ",") + token;
            continue;
        }

        handled = true;
        const std::string::size_type eq = cmd.find('=');
        const std::string behaviour = cmd.substr(0, eq);
        const std::string param = (eq != std::string::npos) ? cmd.substr(eq + 1) : std::string();
        S32 channel = 0;
        const bool is_query = !param.empty() && LLStringUtil::convertToS32(param, channel);

        U32 bit = 0;
        if (behaviour == "slao")
        {
            bit = HOLD_ALL;
        }
        else if (behaviour == "slao:stand")
        {
            bit = HOLD_STAND;
        }
        else if (behaviour == "slao:sit")
        {
            bit = HOLD_SIT;
        }

        if (is_query && (behaviour == "slao" || behaviour == "slao:status"))
        {
            sendStatusReply(channel);
        }
        else if (bit && (param == "n" || param == "add"))
        {
            if (!mHolds.count(object_id) && mHolds.size() >= MAX_HOLDS)
            {
                LL_WARNS() << "AO: too many holds, ignoring " << object_id << LL_ENDL;
                continue;
            }
            Hold& hold = mHolds[object_id];
            hold.mMask |= bit;
            hold.mName = object_name;
            hold.mMissingSince = 0.0;
        }
        else if (bit && (param == "y" || param == "rem"))
        {
            auto it = mHolds.find(object_id);
            if (it != mHolds.end())
            {
                it->second.mMask &= ~bit;
                if (!it->second.mMask)
                {
                    mHolds.erase(it);
                }
            }
        }
        else if (param == "force")
        {
            if (behaviour == "slao:next")
            {
                nextAnim(1);
            }
            else if (sCanToggle && behaviour == "slao:on")
            {
                setEnabled(true);
            }
            else if (sCanToggle && behaviour == "slao:off")
            {
                setEnabled(false);
            }
            else if (sCanToggle && behaviour.compare(0, 9, "slao:set:") == 0)
            {
                selectSetByName(behaviour.substr(9));
            }
        }
    }

    if (!handled)
    {
        return false;
    }
    update();
    notifyChanged();
    if (kept.empty())
    {
        message.clear();
        return true;
    }
    message = "@" + kept;
    return false;
}

void SLAOEngine::releaseHolds(const LLUUID& object_id)
{
    if (mHolds.erase(object_id))
    {
        update();
        notifyChanged();
    }
}

void SLAOEngine::releaseHolds(const LLUUID& object_id, const std::string& filter)
{
    auto it = mHolds.find(object_id);
    if (it == mHolds.end())
    {
        return;
    }
    if (filter.empty())
    {
        releaseHolds(object_id);
        return;
    }
    // "slao" contains e.g. "sla"; "slao:stand" also contains "stand", "slao:sit" "sit".
    const U32 before = it->second.mMask;
    if (std::string("slao").find(filter) != std::string::npos)
    {
        it->second.mMask &= ~HOLD_ALL;
    }
    if (std::string("slao:stand").find(filter) != std::string::npos)
    {
        it->second.mMask &= ~HOLD_STAND;
    }
    if (std::string("slao:sit").find(filter) != std::string::npos)
    {
        it->second.mMask &= ~HOLD_SIT;
    }
    if (it->second.mMask == before)
    {
        return;
    }
    if (!it->second.mMask)
    {
        mHolds.erase(it);
    }
    update();
    notifyChanged();
}

void SLAOEngine::releaseAllHolds()
{
    if (mHolds.empty())
    {
        return;
    }
    LL_INFOS() << "AO: releasing all " << mHolds.size() << " script holds" << LL_ENDL;
    mHolds.clear();
    update();
    notifyChanged();
}

void SLAOEngine::sendStatusReply(S32 channel)
{
    // Each reply is chat from the avatar: a looping script must not make us flood.
    const F64 t = now();
    if (t - mStatusWindowStart > STATUS_REPLY_WINDOW_SECS)
    {
        mStatusWindowStart = t;
        mStatusReplies = 0;
    }
    if (++mStatusReplies > STATUS_REPLY_MAX)
    {
        LL_WARNS_ONCE() << "AO: too many @slao status queries; some are not answered" << LL_ENDL;
        return;
    }
    const SLAOSet* set = getCurrentSet();
    const std::string reply = llformat("slao,%d,%s,%s,%s", PROTOCOL_VERSION,
                                       isEnabled() ? "on" : "off",
                                       (mPauseReason == PAUSE_NONE) ? "active" : "paused",
                                       set ? set->mName.c_str() : "");
    RlvUtil::sendChatReply(channel, reply);
}

//-----------------------------------------------------------------------------
// /ao chat command
//-----------------------------------------------------------------------------

bool SLAOEngine::processChatCommand(const std::string& args)
{
    std::string rest = args;
    LLStringUtil::trim(rest);
    std::string sub = rest;
    std::string arg;
    const std::string::size_type space = rest.find(' ');
    if (space != std::string::npos)
    {
        sub = rest.substr(0, space);
        arg = rest.substr(space + 1);
        LLStringUtil::trim(arg);
    }
    LLStringUtil::toLower(sub);

    auto feedback = [](const std::string& text)
    {
        LLSD fb;
        fb["MESSAGE"] = text;
        LLNotificationsUtil::add("SLAOChatFeedback", fb);
    };

    if (sub.empty() || sub == "toggle")
    {
        toggle();
    }
    else if (sub == "on")
    {
        setEnabled(true);
    }
    else if (sub == "off")
    {
        setEnabled(false);
    }
    else if (sub == "pause")
    {
        setUserPaused(true);
    }
    else if (sub == "resume" || sub == "unpause")
    {
        setUserPaused(false);
        releaseAllHolds();      // the user's say beats any script's hold
    }
    else if (sub == "next")
    {
        nextAnim(1);
    }
    else if (sub == "prev" || sub == "previous")
    {
        nextAnim(-1);
    }
    else if (sub == "set")
    {
        if (arg.empty())
        {
            cycleSet(1);
        }
        else if (!selectSetByName(arg))
        {
            LLStringUtil::format_map_t fmt;
            fmt["[NAME]"] = arg;
            feedback(LLTrans::getString("SLAOChatNoSuchSet", fmt));
            return true;
        }
    }
    else if (sub == "sets")
    {
        std::string names;
        for (const SLAOSet* set : SLAOSets::instance().getSortedSets())
        {
            names += (names.empty() ? "" : ", ") + set->mName;
        }
        LLStringUtil::format_map_t fmt;
        fmt["[SETS]"] = names.empty() ? LLTrans::getString("SLAONoSets") : names;
        feedback(LLTrans::getString("SLAOChatSets", fmt));
        return true;
    }
    else if (sub == "sit")
    {
        std::string mode = arg;
        LLStringUtil::toLower(mode);
        setSitOverride(mode == "on" ? true : (mode == "off" ? false : !getSitOverride()));
    }
    else if (sub == "selftest")
    {
        // Runs a real AO notecard through the parser (see SLAONotecard::selfTest)
        std::string report;
        const bool ok = SLAONotecard::selfTest(report);
        LL_INFOS() << "AO notecard self-test " << (ok ? "passed: " : "FAILED: ") << report << LL_ENDL;
        feedback(std::string(ok ? "notecard self-test passed: " : "notecard self-test FAILED: ") + report);
        return true;
    }
    else if (sub == "status")
    {
        // fall through to the status feedback
    }
    else if (sub == "edit" || sub == "show")
    {
        LLFloaterReg::showInstance("sl_ao_editor");
        return true;
    }
    else
    {
        feedback(LLTrans::getString("SLAOChatUsage"));
        return true;
    }
    feedback(getStatusText());
    return true;
}

//-----------------------------------------------------------------------------
// Status
//-----------------------------------------------------------------------------

std::string SLAOEngine::getCurrentAnimName() const
{
    const SLAOSet* set = getCurrentSet();
    if (!set || mMain.mAnim.isNull())
    {
        return std::string();
    }
    for (const auto& anims : set->mAnims)
    {
        for (const SLAOAnim& anim : anims)
        {
            if (anim.mAssetID == mMain.mAnim)
            {
                return anim.mName;
            }
        }
    }
    return std::string();
}

std::string SLAOEngine::getStatusText() const
{
    std::string status = getBaseStatusText();
    if (!mAlways.empty())
    {
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", (S32)mAlways.size());
        status += LLTrans::getString("SLAOStatusAlwaysSuffix", args);
    }
    if (mBodyFixCount > 0 && isEnabled())
    {
        LLStringUtil::format_map_t args;
        args["[COUNT]"] = llformat("%d", mBodyFixCount);
        status += LLTrans::getString("SLAOStatusBodyFixSuffix", args);
    }
    return status;
}

std::string SLAOEngine::getBaseStatusText() const
{
    LLStringUtil::format_map_t args;
    args["[STATE]"] = stateLabel(mEffectiveState);
    const SLAOSet* set = getCurrentSet();
    args["[SET]"] = set ? set->mName : std::string();

    auto source_name = [this]() -> std::string
    {
        std::string name = attachment_name(gObjectList.findObject(mTakeoverSource));
        return name.empty() ? LLTrans::getString("SLAOUnknownScript") : name;
    };

    switch (mPauseReason)
    {
    case PAUSE_DISABLED:
        return LLTrans::getString("SLAOStatusOff");
    case PAUSE_USER:
        return LLTrans::getString("SLAOStatusUserPaused");
    case PAUSE_NO_SET:
        return LLTrans::getString("SLAOStatusNoSet");
    case PAUSE_LOADING:
        return LLTrans::getString("SLAOStatusLoading");
    case PAUSE_SCRIPTED_AO:
        return LLTrans::getString("SLAOStatusScriptedAO");
    case PAUSE_SCRIPT_HOLD:
    {
        std::string holder;
        for (const auto& [id, hold] : mHolds)
        {
            holder = hold.mName;
            break;
        }
        args["[NAME]"] = holder.empty() ? LLTrans::getString("SLAOUnknownScript") : holder;
        return LLTrans::getString("SLAOStatusScriptHold", args);
    }
    case PAUSE_SCRIPT_ANIM:
        args["[NAME]"] = source_name();
        return LLTrans::getString("SLAOStatusScriptAnim", args);
    case PAUSE_SERVER_AO:
        return LLTrans::getString("SLAOStatusServerAO", args);
    case PAUSE_SMART_SIT:
        return LLTrans::getString("SLAOStatusSmartSit");
    case PAUSE_SIT_WAIT:
        return LLTrans::getString("SLAOStatusSitWait");
    case PAUSE_SIT_OFF:
        return LLTrans::getString("SLAOStatusSitOff");
    case PAUSE_RLV_SIT:
        return LLTrans::getString("SLAOStatusRlvSit");
    case PAUSE_MOUSELOOK:
        return LLTrans::getString("SLAOStatusMouselook");
    case PAUSE_TYPING:
        return LLTrans::getString("SLAOStatusTyping");
    case PAUSE_EMPTY:
        return LLTrans::getString("SLAOStatusEmpty", args);
    case PAUSE_NONE:
    default:
        break;
    }
    if (mMain.mAnim.notNull())
    {
        args["[STATE]"] = stateLabel(mIdleAnim.notNull() ? IDLE : mMain.mState);
        args["[NAME]"] = getCurrentAnimName();
        return LLTrans::getString("SLAOStatusPlaying", args);
    }
    return LLTrans::getString("SLAOStatusActive", args);
}

void SLAOEngine::notifyChanged()
{
    mChangedSignal();
}
