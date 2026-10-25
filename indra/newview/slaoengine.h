/**
 * @file slaoengine.h
 * @brief Client-side animation overrider (#496): the runtime. Watches the
 *        agent's signaled animation state and plays the active set's
 *        replacements, steps aside for scripts, poseballs, collars and other
 *        AOs, and takes commands from chat and from the agent's own objects.
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

#ifndef SL_AOENGINE_H
#define SL_AOENGINE_H

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/signals2.hpp>

#include "llsingleton.h"
#include "lluuid.h"
#include "slaoset.h"

class SLAOEngine : public LLSingleton<SLAOEngine>
{
    LLSINGLETON(SLAOEngine);
    ~SLAOEngine() override;
    LOG_CLASS(SLAOEngine);

public:
    // Why a state is not being overridden right now (first match wins).
    enum EPause
    {
        PAUSE_NONE = 0,
        PAUSE_DISABLED,         // AO switched off
        PAUSE_USER,             // paused by hand (/ao pause, compact control)
        PAUSE_NO_SET,           // no set selected / set not found
        PAUSE_LOADING,          // waiting for inventory or the region
        PAUSE_SCRIPTED_AO,      // a worn scripted AO the user chose to defer to
        PAUSE_SCRIPT_HOLD,      // an owned object asked us to pause (@slao)
        PAUSE_SCRIPT_ANIM,      // a script animation (pose, dance, poseball) owns the body
        PAUSE_SMART_SIT,        // the seat animates the avatar
        PAUSE_SIT_WAIT,         // just sat down; waiting to see if the seat animates us
        PAUSE_SIT_OFF,          // sit override switched off for this set
        PAUSE_RLV_SIT,          // RLV-locked sit with its own animation
        PAUSE_MOUSELOOK,        // stands disabled in mouselook
        PAUSE_TYPING,           // stands disabled while typing
        PAUSE_SERVER_AO,        // a script overrides this state server-side (llSetAnimationOverride)
        PAUSE_EMPTY,            // the set has no animation for this state
    };

    // Called after login once settings and the per-account directory exist.
    void init();
    void shutdown();

    bool isEnabled() const;
    void setEnabled(bool enabled);
    void toggle() { setEnabled(!isEnabled()); }

    bool isUserPaused() const { return mUserPaused; }
    void setUserPaused(bool paused);

    const LLUUID& getCurrentSetID() const { return mCurrentSetID; }
    const SLAOSet* getCurrentSet() const;
    void selectSet(const LLUUID& set_id);
    bool selectSetByName(const std::string& name);
    void cycleSet(S32 delta);

    void nextAnim(S32 delta = 1);   // next/previous stand (or current state)

    // True while the AO is active and would supply the walking animation
    // (enabled, not paused, the set has a Walking or Walking Backwards one).
    // LLVOAvatar::updateOrientation uses it for "Walk backwards facing forward".
    bool overridesWalk() const;

    bool getSitOverride() const;
    void setSitOverride(bool enabled);

    // Status for the UI
    SLAO::EState getCurrentState() const { return mMain.mState; }
    SLAO::EState getDetectedState() const { return mEffectiveState; }
    const LLUUID& getCurrentAnim() const { return mMain.mAnim; }
    std::string getCurrentAnimName() const;
    EPause getPauseReason() const { return mPauseReason; }
    std::string getStatusText() const;
    // The animation (and its source) that pauses the AO right now, if any
    const LLUUID& getTakeoverAnim() const { return mTakeoverAnim; }
    std::string getTakeoverSourceName() const;
    // "Don't pause for this": add the current takeover animation to the
    // per-account ignore list
    void ignoreTakeoverAnim();
    // "Resume" while deferring to a worn scripted AO: switch every worn
    // "pause" choice to "keep both" so the AO runs again
    void resumeFromScriptedAO();
    S32 getBodyFixCount() const { return mBodyFixCount; }
    // True while the AO holds the head still: no procedural head turning on
    // our avatar, and no look-at target sent to other viewers
    bool holdsHead() const { return mHoldingHead; }
    bool isIdlePlaying() const { return mIdleAnim.notNull(); }
    bool isAlwaysPlaying(const LLUUID& asset_id) const { return mAlways.count(asset_id) != 0; }
    S32 getAlwaysCount() const { return (S32)mAlways.size(); }
    bool isAnimationBad(const LLUUID& asset_id) const { return mBadAnims.count(asset_id) != 0; }
    // Loaded and below the priority that reliably beats the built-ins
    bool isLowPriority(const LLUUID& asset_id) const;

    // Local-only preview for the editor (never sent to the server).
    void previewAnimation(const LLUUID& asset_id);
    void stopPreview();
    const LLUUID& getPreviewAnim() const { return mPreviewAnim; }

    // Hook: self AvatarAnimation message, after LLVOAvatar processed it.
    // Pairs are (animation id, source object id), as the server sent them.
    void onSelfAnimations(const std::vector<std::pair<LLUUID, LLUUID>>& anims);

    // Hook: this frame's movement controls, just before the agent update is
    // sent. Used to predict locomotion state changes one round trip early.
    void onControls(U32 control_flags);

    // Seamless state changes (#668, setting SLAOSyncIntercept).
    // True when the self AvatarAnimation list should reach the AO before the
    // avatar starts the motions in it, so the hooks below decide on the new
    // state rather than the previous one.
    bool wantsAnimationsFirst() const;
    // Hook: LLVOAvatar::startMotion on our avatar. True: do not start this
    // built-in state motion locally; the AO's replacement already covers it,
    // so no frame of the default ever shows. The avatar still records it as
    // playing and gets the matching stop from the server.
    bool interceptStockStart(const LLUUID& anim_id);
    // Hook: LLVOAvatar::stopMotion on our avatar (bookkeeping only).
    void onStockStop(const LLUUID& anim_id);
    // True while the AO keeps this built-in off our avatar. The avatar's
    // playing-animation repair and Refresh Animations must not restart it.
    bool suppressesStockMotion(const LLUUID& anim_id) const;
    // A posture change the viewer itself made (simulated swimming switching
    // flight on or off): locomotion prediction may cross between the ground
    // and air states for a moment, and the server's echo of the change does
    // not swap the replacement back and forth.
    void noteOwnTransition();

    // Hook: llOwnerSay text from an object owned by the agent. Removes the
    // "@slao..." commands it handles from `message`; returns true if nothing
    // is left to display or pass on to RLVa.
    bool processOwnerSay(const LLUUID& object_id, const std::string& object_name, std::string& message);

    // /ao chat command; returns true if handled.
    bool processChatCommand(const std::string& args);

    typedef boost::signals2::signal<void()> changed_signal_t;
    boost::signals2::connection setChangedCallback(const changed_signal_t::slot_type& cb)
    {
        return mChangedSignal.connect(cb);
    }

private:
    struct Channel
    {
        SLAO::EState mState = SLAO::STATE_NONE;
        LLUUID  mAnim;
        F64     mSentAt = 0.0;
        S32     mRetries = 0;
        bool    mConfirmed = false;     // seen in the server's animation list
    };

    // Classification of animations started by others
    // CLS_BODY: poses the body (rotates pelvis/torso/legs away from rest).
    // CLS_BODY_FIX: touches body joints without posing them (deformers,
    // bone-offset fixes: position keys only, or rest rotations).
    enum EClass { CLS_PENDING, CLS_BODY, CLS_PARTIAL, CLS_BODY_FIX };
    struct ClassInfo
    {
        EClass mClass = CLS_PENDING;
        S32    mPosePriority = 0;   // highest priority on the body joints it rotates
    };

    struct Hold
    {
        U32 mMask = 0;          // HOLD_* bits
        std::string mName;      // object name, for the status line
        F64 mMissingSince = 0.0;
    };
    enum
    {
        HOLD_ALL = 1 << 0,
        HOLD_STAND = 1 << 1,
        HOLD_SIT = 1 << 2,
    };

    struct ScriptedAOScore
    {
        S32 mHits = 0;
        F64 mFirstHit = 0.0;
        std::set<LLUUID> mAnims;    // distinct animations seen
        std::string mName;
    };

    void onIdle();
    void onTick();
    void onSetsChanged();
    void onRegionChanged();
    void onSettingChanged();

    void update();
    SLAO::EState computeSignaledState() const;
    SLAO::EState predictState() const;
    SLAO::EState effectiveState();
    EPause pauseFor(SLAO::EState state) const;
    LLUUID pickAnim(SLAO::EState state, bool entering);
    void apply(Channel& channel, SLAO::EState state, const LLUUID& anim, bool main_channel, bool user_initiated);
    void stopAll(bool restore_defaults);
    void sendRequests(const std::vector<std::pair<LLUUID, bool>>& requests);
    bool stopSystemAnimsEnabled() const;
    bool mayStopBuiltin(const LLUUID& builtin, std::vector<std::pair<LLUUID, bool>>& requests);
    void stopSystemAnims(SLAO::EState state, std::vector<std::pair<LLUUID, bool>>& requests);
    void restoreSystemAnims(std::vector<std::pair<LLUUID, bool>>& requests);
    bool isTurning() const;
    void trackYaw();
    void reconcile();
    std::string getBaseStatusText() const;

    void updateAlways();
    void updateHeadHold();
    void reconcileAlways();

    bool syncInterceptEnabled() const;
    // The built-in of this state stays under the replacement (not stopped,
    // locally or on the server) in sync mode
    bool keepsStockUnder(SLAO::EState stock_state) const;
    // Our avatar's local built-in motions to match the current decision
    void syncLocalStock();
    // Replacements we stopped that are still in the server's list
    void noteOwnStop(const LLUUID& anim_id);
    void cleanupStrays();
    // Region arrival: the current replacements, right away
    void resendOnArrival();

    bool isIdleHost(SLAO::EState state) const;
    bool idleAllowed() const;
    void scheduleIdle();
    void startIdle();
    void endIdle(bool reschedule);
    void tickIdle();
    void updateSeat();

    EClass classify(const LLUUID& anim_id);
    // Effective priority of the body joints a foreign animation rotates (-1: unknown)
    S32 posePriority(const LLUUID& anim_id);
    // Priority of our replacement on the posture channel (-1: unknown)
    S32 ownPriority() const;
    bool isAlwaysAssetAnywhere(const LLUUID& asset_id) const;
    void scanForeign();
    void noteScriptedAOActivity(const LLUUID& anim_id, const LLUUID& source_id);
    void offerScriptedAOPause(const LLUUID& item_id, const std::string& name);
    bool isScriptedAOWorn() const;
    bool isSetAnim(const LLUUID& asset_id) const;

    void preloadSet();
    void onAssetFetched(const LLUUID& asset_id, S32 status);
    void checkPreloads();

    void releaseHolds(const LLUUID& object_id);
    void releaseHolds(const LLUUID& object_id, const std::string& filter);
    void releaseAllHolds();
    void sendStatusReply(S32 channel);
    void notifyChanged();

    bool mInitialized = false;
    bool mUserPaused = false;
    LLUUID mCurrentSetID;

    // Last animation list from the server: anim -> source
    std::map<LLUUID, LLUUID> mSignaled;
    SLAO::EState mSignaledState = SLAO::STATE_NONE;
    SLAO::EState mEffectiveState = SLAO::STATE_NONE;
    bool mTypingSignaled = false;
    F64 mSignaledChangedAt = 0.0;

    // Prediction from local controls
    U32 mLastControlMask = 0;
    bool mLastFlying = false;
    F64 mControlsChangedAt = 0.0;
    F64 mAirEnteredAt = 0.0;    // last change from a ground state to hover/fly

    struct AlwaysEntry
    {
        F64  mSentAt = 0.0;
        S32  mRetries = 0;
        bool mConfirmed = false;
    };
    std::map<LLUUID, AlwaysEntry> mAlways;  // Always animations we keep running

    LLUUID mIdleAnim;           // idle playing in place of the stand
    LLUUID mLastIdle;
    F64 mIdleUntil = 0.0;
    F64 mNextIdleAt = 0.0;      // 0: not scheduled

    Channel mMain;
    Channel mOverlay;   // typing
    S32 mIndex[SLAO::STATE_COUNT];          // current animation per state (session)
    bool mEverPicked[SLAO::STATE_COUNT];    // entered at least once with this set
    bool mManualPick[SLAO::STATE_COUNT];    // user chose the animation with prev/next; kept on re-entry when Cycle is off
    F64 mStateEnteredAt = 0.0;
    F64 mLastCycleAt = 0.0;

    // Built-ins we asked the server to stop, with their state
    std::map<LLUUID, SLAO::EState> mStoppedDefaults;
    bool mStoppedAway = false;
    bool mStoppedTyping = false;
    bool mHoldingHead = false;
    std::set<LLUUID> mWarnedLowPriority;

    // Turning from the avatar's rotation (mouselook steering)
    F32 mLastYaw = 0.f;
    F64 mYawTime = 0.0;
    F32 mYawRate = 0.f;         // radians per second, smoothed
    S32 mYawTurn = 0;           // 1 left, -1 right, 0 not turning
    F64 mYawQuietSince = 0.0;
    std::map<LLUUID, std::vector<F64>> mDefaultStopTimes; // rate limit
    // Animations we stopped recently (may still be in the server's list)
    std::map<LLUUID, F64> mRecentlyStopped;

    // Foreign animations (not built in, not ours)
    std::map<LLUUID, ClassInfo> mClassCache;
    S32 mBodyFixCount = 0;                  // foreign body fixes playing now (ignored)
    std::set<LLUUID> mLoggedBodyFixes;
    S32 mLastOwnPriority = -1;
    bool mBodyTakeover = false;
    bool mSeatAnimates = false;
    bool mServerOverride = false;
    LLUUID mTakeoverSource;
    LLUUID mTakeoverAnim;
    std::set<LLUUID> mAnimatingSeats;
    LLUUID mSeatID;
    F64 mSatAt = 0.0;
    F64 mSitSignaledAt = 0.0;
    F64 mStateChangedAt = 0.0;  // last signaled posture change (for scripted AO detection)

    std::map<LLUUID, ScriptedAOScore> mScriptedAOScores;   // attachment item -> score
    std::set<LLUUID> mScriptedAOOffered;                   // per session

    std::map<LLUUID, Hold> mHolds;  // object id -> hold
    F64 mStatusWindowStart = 0.0;   // @slao status reply budget
    S32 mStatusReplies = 0;

    // Preload / failure tracking
    std::map<LLUUID, F64> mPreloading;      // played, not loaded yet
    std::set<LLUUID> mFetchRequested;       // asset fetches asked this session
    std::set<LLUUID> mBadAnims;

    LLUUID mPreviewAnim;

    // Sync intercept (#668)
    std::set<LLUUID> mSuppressed;   // built-ins (as signaled) kept off our avatar locally
    bool mInUpdate = false;         // our own start/stop calls pass the hooks untouched
    F64 mOwnTransitionAt = 0.0;
    struct OwnStop
    {
        F64 mSentAt = 0.0;
        S32 mRetries = 0;
    };
    std::map<LLUUID, OwnStop> mOwnStopped;  // replacements we asked the server to stop

    EPause mPauseReason = PAUSE_DISABLED;
    std::string mPauseDetail;

    F64 mLastTick = 0.0;
    boost::signals2::connection mSetsConnection;
    boost::signals2::connection mRegionConnection;
    boost::signals2::connection mEnabledConnection;
    boost::signals2::connection mSetConnection;
    changed_signal_t mChangedSignal;
};

#endif // SL_AOENGINE_H
