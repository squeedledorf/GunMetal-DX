/**
 * @file slposelookat.h
 * @brief Pose Studio look-at sync (#902): where an avatar looks, shared between
 *        Slipstream viewers over the pose-sync channel and applied as a procedural
 *        layer to the body's own neck / head chain (Bento necks, pony and other
 *        quadruped rigs, mesh heads that animations would otherwise hold still).
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

#ifndef SL_POSELOOKAT_H
#define SL_POSELOOKAT_H

#include <array>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/signals2.hpp>

#include "llmotion.h"
#include "llquaternion.h"
#include "llsingleton.h"
#include "lltimer.h"
#include "v3math.h"
#include "lluuid.h"

class LLEventTimer;
class LLVOAvatar;

/**
 * Wire format (rides the pose channel as a pose-line kind, so the helper forwards and
 * budgets it like any other pose line and no helper update is needed):
 *
 *   P1|<seq>|L|1/1|-|<data>
 *     target is always "-": a look-at only ever describes the sender's own avatar, whose
 *     identity the receiving helper took from the sim (owner of the speaking attachment).
 *     data = base64url of
 *       U8  version (1)
 *       U8  flags: 0 = no ease sent (receivers use their own setting); otherwise the
 *           sender's look-at ease (SLLookAtSyncEase) is (flags - 1) * 0.02 seconds, 0..5.08
 *           (old receivers ignore this byte)
 *       S16 yaw, hundredths of a degree, big-endian: + = to the avatar's left
 *       S16 pitch, hundredths of a degree, big-endian: + = up
 *           both in the avatar's own frame (its root rotation), from the head to the target
 *       U8  weight 0..255 (0 = not looking at anything: blend out)
 *       U8  length of the bone map id, then the id: [a-z0-9_], at most MAX_MAP_ID bytes
 *
 * Rates: the sender samples its target every frame (shown on its own avatar at once) and
 * the network side every TICK_PERIOD; sent when the direction moved by SEND_ANGLE or the
 * weight / map changed, at most once per MIN_SEND_INTERVAL; while looking, a keep-alive
 * every KEEPALIVE_INTERVAL; one weight-0 line when the look-at ends. Receivers fade an
 * avatar out after STALE_AFTER without a line.
 *
 * Viewers that predate this ignore the unknown kind (1.4.0-beta.3 and later drop it before
 * keeping any state; 1.3.x keeps an inert, invisible remote slot per sender until it goes
 * quiet).
 */

// One bone of a look-at chain (lookat_maps.xml). Angles in radians.
struct SLLookAtBone
{
    std::string mJoint;
    F32 mYawShare = 0.f;        // eyes only: share of the look-at this bone aims with (chain bones are
    F32 mPitchShare = 0.f;      // filled by priority, head first; the shares are kept for old maps)
    S32 mPriority = 0;          // lower fills first (head 0, neck 10, chest 20, ...); not for eyes
    F32 mYawLimit = 0.f;        // the most this bone turns, either way
    F32 mPitchUpLimit = 0.f;
    F32 mPitchDownLimit = 0.f;
    bool mEye = false;          // eyes: aim with what the chain could not reach, do not consume it
};

struct SLLookAtMap
{
    static constexpr size_t MAX_BONES = 12;

    std::string mId;
    std::string mLabel;
    std::string mFigure;                // Pose Studio figure that implies this map (auto)
    std::vector<std::string> mDetect;   // bones that must all be rigged for auto to pick it
    std::vector<std::string> mDetectAny;    // ... and at least one of these (when listed)
    std::vector<std::string> mDetectNames;  // worn item names containing one of these (lower case)
    std::vector<std::string> mDetectNamesExclude;   // ... unless the name contains one of these
    std::vector<SLLookAtBone> mBones;   // root -> tip; eyes last
    std::vector<size_t> mOrder;         // non-eye bone indices, highest priority (head) first
    void buildOrder();
    bool drives(const std::string& joint) const;
};

// The procedural layer: an additive motion on the avatar, run after the animations it sits
// on. One instance per avatar; it reads its drive from SLPoseLookAt every frame.
class SLPoseLookAtMotion : public LLMotion
{
public:
    SLPoseLookAtMotion(const LLUUID& id);
    ~SLPoseLookAtMotion() override;

    static LLMotion* create(const LLUUID& id) { return new SLPoseLookAtMotion(id); }

    bool getLoop() override { return true; }
    F32 getDuration() override { return 0.f; }
    F32 getEaseInDuration() override { return 0.f; }
    F32 getEaseOutDuration() override { return 0.f; }
    // ADDITIVE_PRIORITY (#902): an additive state only adds what the regular states above it
    // left unweighted (LLJointStateBlender::blendJointStates), so below an AO's priority 3-6
    // stand that holds the neck / head at full weight it added exactly nothing. An animation's
    // priority is clamped below ADDITIVE_PRIORITY, and the additive pass runs before the
    // regular one, so at this priority the layer's state sorts first and always applies.
    LLJoint::JointPriority getPriority() override { return LLJoint::ADDITIVE_PRIORITY; }
    LLMotionBlendType getBlendType() override { return ADDITIVE_BLEND; }
    F32 getMinPixelArea() override;
    LLMotionInitStatus onInitialize(LLCharacter* character) override;
    bool onActivate() override;
    bool onUpdate(F32 time, U8* joint_mask) override;
    void onDeactivate() override;

    // Faded all the way out with nothing driving it: SLPoseLookAt may stop it.
    bool isIdle() const { return mIdle; }
    // Diagnostics: the smoothed weight, and the total angle (radians) the layer added to its
    // bones on the last update.
    F32 getAppliedWeight() const { return mWeight; }
    F32 getAppliedAngle() const { return mAppliedAngle; }
    S32 getBoundBones() const;
    F32 getYaw() const { return mYaw; }
    F32 getPitch() const { return mPitch; }
    // Diagnostics: "joint=yaw/pitch,..." in degrees, what each map bone got on the last update.
    std::string describeBones() const;
    // Diagnostics / watchdog (#902): the motion found this skeleton's root and bones at init
    // (a motion initialised against an incomplete skeleton does nothing for its whole life),
    // how many bone slots it holds, the weight its last drive asked for, and how long since
    // the motion controller last ran onUpdate (LOD, off-screen and rate throttles skip it).
    bool isBound() const { return mRootJoint && !mSlots.empty(); }
    S32 getSlotCount() const { return (S32)mSlots.size(); }
    F32 getTargetWeight() const { return mTargetWeight; }
    F32 secondsSinceUpdate() const { return mUpdates ? mSinceUpdate.getElapsedTimeF32().value() : -1.f; }
    U32 getUpdateCount() const { return mUpdates; }

private:
    struct Slot
    {
        std::string mName;
        LLJoint* mJoint = nullptr;
        LLPointer<LLJointState> mState;
        LLQuaternion mCumLast;      // our world-space change on this bone last frame
        F32 mOwn = 0.f;             // smoothed 1 = ours, 0 = not in the map / owned by a pose
    };
    void bindMap(const SLLookAtMap* map);

    LLCharacter* mCharacter = nullptr;
    LLJoint* mRootJoint = nullptr;
    LLUUID mAvatarID;
    std::vector<Slot> mSlots;               // the union of every map's bones on this skeleton
    const SLLookAtMap* mBoundMap = nullptr;
    std::vector<S32> mMapSlot;              // map bone -> slot (-1: not on this skeleton)
    std::vector<S32> mMapParent;            // map bone -> nearest ancestor map bone (-1: none)
    F32 mYaw = 0.f;
    F32 mPitch = 0.f;
    F32 mWeight = 0.f;
    F32 mYawVel = 0.f;                      // critically damped easing state (radians / second)
    F32 mPitchVel = 0.f;
    F32 mWeightVel = 0.f;
    F32 mEase = 1.f;                        // seconds; the last drive's
    std::vector<F32> mAppliedYaw;           // by map bone, radians, last update
    std::vector<F32> mAppliedPitch;
    F32 mLastTime = -1.f;
    F32 mAppliedAngle = 0.f;
    F32 mTargetWeight = 0.f;                // the drive's weight on the last update
    LLTimer mSinceUpdate;                   // reset by every onUpdate
    U32 mUpdates = 0;
    bool mIdle = true;
};

class SLPoseLookAt : public LLSingleton<SLPoseLookAt>
{
    LLSINGLETON(SLPoseLookAt);
    ~SLPoseLookAt() override;
    LOG_CLASS(SLPoseLookAt);

public:
    static const LLUUID ANIM_ID;
    static constexpr size_t MAX_MAP_ID = 24;

    // After login (needs settings, the bridge and the presets).
    void init();

    // ---- Presets ---------------------------------------------------------
    const std::vector<SLLookAtMap>& getMaps() const { return mMaps; }
    const SLLookAtMap* findMap(const std::string& id) const;
    // The union of every map's bones (what a motion holds joint states for).
    const std::vector<std::string>& getAllBones() const { return mAllBones; }
    // Our own map: the setting, or for "auto" what detectMap last found.
    std::string getOwnMapId() const;
    std::string getDetectedMapId() const { return mDetectedMap; }

    // ---- Sender state (Pose Studio, diagnostics) ----------------------------
    enum EBlock
    {
        BLOCK_NONE = 0,         // sending (or ready to: nothing changed)
        BLOCK_OFF,              // "Send where I look" is off
        BLOCK_RLV,              // RLV restricts the pose channel
        BLOCK_BRIDGE,           // the helper is not ready
        BLOCK_PRIVACY,          // "Don't send my Look At target to others" (EnableLookAtTarget)
        BLOCK_LOOKAT_DISABLED,  // "Disable Look At Animation" (DisableLookAtAnimation)
        BLOCK_AO,               // the client AO holds the head
        BLOCK_NO_TARGET,        // LL's look-at has no target right now
        BLOCK_COUNT
    };
    EBlock getBlockReason() const { return mBlock; }
    static const char* blockName(EBlock block);

    // ---- People ----------------------------------------------------------
    // Avatars whose look-at we currently show (not ourselves), and the muted ones.
    std::vector<LLUUID> getSenders() const;
    bool isMuted(const LLUUID& avatar_id) const { return mMuted.count(avatar_id) != 0; }
    std::vector<LLUUID> getMutedList() const { return std::vector<LLUUID>(mMuted.begin(), mMuted.end()); }
    void setMuted(const LLUUID& avatar_id, bool muted);

    // ---- Receiving (from SLPoseSync: a P1 line of kind L) -----------------
    void onRemoteLine(const LLUUID& sender, const std::string& parts, const std::string& target, const std::string& data);

    // ---- The motion's drive -----------------------------------------------
    struct Drive
    {
        F32 mYaw = 0.f;         // radians, avatar frame
        F32 mPitch = 0.f;
        F32 mWeight = 0.f;      // 0..1, staleness and behind-the-head fading applied
        F32 mLimitScale = 1.f;
        F32 mEase = 1.f;        // seconds: the sender's, else our setting
        const SLLookAtMap* mMap = nullptr;
        std::array<bool, SLLookAtMap::MAX_BONES> mPosed{};    // owned by a Pose Studio pose
    };
    bool getDrive(const LLUUID& avatar_id, Drive& out) const;
    // True while the layer turns this avatar's head: LL's own head turn (head_rot) stands
    // down meanwhile, or the head would turn twice.
    bool isDriving(const LLUUID& avatar_id) const;

    typedef boost::signals2::signal<void()> changed_signal_t;
    boost::signals2::connection setChangedCallback(const changed_signal_t::slot_type& cb) { return mChangedSignal.connect(cb); }

private:
    struct Entry
    {
        F32 mYaw = 0.f;
        F32 mPitch = 0.f;
        F32 mWeight = 0.f;
        F32 mEase = -1.f;       // seconds the sender asked for; < 0: use our setting
        const SLLookAtMap* mMap = nullptr;
        std::array<bool, SLLookAtMap::MAX_BONES> mPosed{};
        LLTimer mLastHeard;
        F32 mTokens = 0.f;      // per-sender rate limit (token bucket)
        LLTimer mTokenTimer;
        // Diagnostics / watchdog (#902)
        std::string mDeclaredMap;   // the map id the sender named (before the human fallback)
        U32 mLines = 0;             // lines accepted from this sender
        F32 mStuckFor = 0.f;        // seconds driven while the motion applied nothing
        U32 mRestarts = 0;          // watchdog restarts of this avatar's motion
        std::string mLastRestart;   // why the last one happened
    };

    // Lines from others that onRemoteLine dropped, by stage (diagnostics, #902).
    enum EReject
    {
        REJ_NOT_INIT = 0,       // before init / no sender / ourselves
        REJ_SHOW_OFF,           // "Show look-at" or "Show poses" off
        REJ_FORMAT,             // parts / target / size / base64 / header / map id
        REJ_VERSION,            // a later wire version
        REJ_MUTED,              // muted (look-at list or the viewer mute list)
        REJ_NO_AVATAR,          // the sender's avatar is not loaded or not in our region
        REJ_FULL,               // MAX_ENTRIES people shown already
        REJ_RATE,               // the per-sender rate limit
        REJ_COUNT
    };
    static const char* rejectName(EReject reason);
    void noteReject(const LLUUID& sender, EReject reason);
    std::string anonId(const LLUUID& id) const;
    std::string describeEntry(const LLUUID& id, const Entry& entry);
    void watchMotion(LLVOAvatar* avatar, Entry& entry, F32 dt);
    void restartMotion(LLVOAvatar* avatar, Entry& entry, const char* why, bool replace);
    void tickSendStatus(const std::string& why);

    void loadMaps();
    void tick();
    void tickFast();
    void sampleSelf();
    void tickSend();
    void tickStatus();
    void logStatus(const char* why);
    void tickMotions();
    void detectMap();
    bool isRigged(const std::string& joint) const;
    // The map the rigging of this avatar's worn meshes implies (the self detection's
    // rigging step, for any avatar; diagnostics for remote avatars), and per map bone
    // whether that avatar's meshes are rigged to it.
    std::string riggingMapFor(LLVOAvatar* avatar, const std::vector<bool>& rigged, S32 rigged_count) const;
    static bool isRiggedOn(LLVOAvatar* avatar, const std::vector<bool>& rigged, const std::string& joint);
    void sendLine(F32 yaw, F32 pitch, F32 weight, const std::string& map_id, U8 ease_code);
    void dropEntry(const LLUUID& avatar_id);
    void updatePosed(LLVOAvatar* avatar, Entry& entry);
    F32 effectiveWeight(const Entry& entry) const;
    LLVOAvatar* findAvatar(const LLUUID& id) const;
    void ensureMotion(LLVOAvatar* avatar);
    void saveMuted() const;

    std::vector<SLLookAtMap> mMaps;     // loaded once; pointers into it stay valid
    std::vector<std::string> mAllBones;
    std::string mDetectedMap;
    std::string mDetectedHow;           // figure / rigging / names / default (diagnostics)
    std::vector<bool> mRigged;          // by joint number: the worn meshes weight this bone
    S32 mRiggedCount = 0;
    LLTimer mDetectTimer;
    bool mDetected = false;

    std::map<LLUUID, Entry> mEntries;   // gAgentID: our own, as we last sent it
    std::set<LLUUID> mMuted;
    std::set<LLUUID> mMotionsRunning;   // avatars our motion runs on
    std::set<LLUUID> mHeadRotStopped;   // avatars whose head_rot we stood down

    // Sender: the live target (sampled every frame) and what last went out
    bool mSendingNow = false;           // tickSend found sending possible: tickFast samples
    F32 mLiveYaw = 0.f;                 // the last direction while looking; held when not
    F32 mLivePitch = 0.f;
    F32 mLiveWeight = 0.f;
    U8 mSentEase = 0;
    bool mSentActive = false;           // the last line we sent had weight > 0
    F32 mSentYaw = 0.f;
    F32 mSentPitch = 0.f;
    F32 mSentWeight = 0.f;
    std::string mSentMap;
    LLTimer mLastSend;
    bool mEverSent = false;
    U16 mSeq = 0;
    bool mBridgeUser = false;

    // Diagnostics (#PoseLookAt status line)
    EBlock mBlock = BLOCK_OFF;
    S32 mLLType = -1;                   // LL's look-at type at the last sample (-1: no effect)
    F32 mLLDistance = -1.f;             // its distance from the head, metres
    // #902: where the camera is and looks, against LL's target (region-local metres), and the
    // angle between the camera's forward axis and head-to-target (-1: no target).
    LLVector3 mCamPosRegion;
    LLVector3 mCamFocusRegion;
    LLVector3 mLLTargetRegion;
    F32 mCamTargetAngle = -1.f;
    bool mFreeCam = false;              // the free-camera look-at owns the target
    U32 mSendsInWindow = 0;
    U32 mBacklogWaits = 0;
    U32 mRxLines = 0;                   // others' look-at lines received (cumulative)
    U32 mRejects[REJ_COUNT] = {};       // ... dropped, by stage (cumulative)
    std::map<LLUUID, std::array<U32, REJ_COUNT>> mRejectsBy;    // ... by sender (bounded)
    LLTimer mWatchTimer;                // watchdog clock (tick period)
    // Sender diagnostics: why nothing goes out (logged when it has lasted a while)
    LLTimer mNotSendingTimer;           // since sending last worked or was not wanted
    std::string mNotSendingWhy;         // the current reason ("" while sending works)
    LLTimer mNotSendingLogTimer;
    bool mNotSendingLogged = false;
    LLTimer mBacklogTimer;              // since a wanted line first waited on the backlog
    bool mBacklogWaiting = false;
    LLTimer mSendWindow;
    LLTimer mStatusTimer;
    std::string mStatusKey;             // what changed-state logging compares

    bool mInitialized = false;
    LLEventTimer* mTicker = nullptr;
    LLEventTimer* mFastTicker = nullptr;
    changed_signal_t mChangedSignal;
};

#endif // SL_POSELOOKAT_H
