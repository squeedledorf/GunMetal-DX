/**
 * @file slposestudio.h
 * @brief Pose Studio (#500): the one posing engine every poser UI and the pose
 *        sync drive -- target selection, swing-twist bone edits, mirror,
 *        freeze, the pose library and sequenced playback.
 *
 * The joint-override engine underneath (FSPoserAnimator / FSPosingMotion /
 * FSJointPose) is Firestorm's poser by Angeldark Raymaker, kept as-is; the
 * pose file reader/writer below started as FSFloaterPoser's and still reads
 * and writes the same XML (version 8), so poses saved by either UI load in
 * both.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * Portions Copyright (c) 2024 Angeldark Raymaker @ Second Life (Firestorm)
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

#ifndef SL_POSESTUDIO_H
#define SL_POSESTUDIO_H

#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/signals2.hpp>

#include "fsposeranimator.h"
#include "llsingleton.h"
#include "lltimer.h"
#include "lluuid.h"
#include "slposemath.h"

class LLEventTimer;
class LLVOAvatar;

// How much of a saved pose to apply (shared with the classic FS poser floater).
typedef enum E_LoadPoseMethods
{
    ROTATIONS               = 1,
    POSITIONS               = 2,
    SCALES                  = 3,
    ROTATIONS_AND_POSITIONS = 4,
    ROTATIONS_AND_SCALES    = 5,
    POSITIONS_AND_SCALES    = 6,
    ROT_POS_AND_SCALES      = 7,
    HAND_RIGHT              = 8,
    HAND_LEFT               = 9,
    FACE_ONLY               = 10,
    SELECTIVE               = 11,
    SELECTIVE_ROT           = 12,
} E_LoadPoseMethods;

class SLPoseStudio : public LLSingleton<SLPoseStudio>
{
    LLSINGLETON(SLPoseStudio);
    ~SLPoseStudio() override;
    LOG_CLASS(SLPoseStudio);

public:
    typedef FSPoserAnimator::FSPoserJoint joint_t;

    struct SequenceEntry
    {
        std::string mPose;
        F32 mHold = 3.f;    // seconds this pose is held before the next one
        F32 mEase = -1.f;   // ease-in override in seconds; < 0 = the pose file's own
    };

    static constexpr F32 DEFAULT_EASE_IN = 0.5f;    // for pose files that do not say
    static constexpr F32 MAX_EASE_IN = 5.f;

    // The single shared engine. FSPoserAnimator keeps per-instance capture state for
    // restoring an avatar when posing stops, so there must only ever be one.
    FSPoserAnimator& getAnimator() { return mAnimator; }

    // ---- Target ----------------------------------------------------------
    const LLUUID& getTargetID() const { return mTargetID; }
    LLVOAvatar* getTarget() const;
    void setTarget(const LLUUID& avatar_id);
    // Self, an animesh you own, or (later) someone with a verified consent grant.
    bool canPose(LLVOAvatar* avatar) const;
    bool isPosing(LLVOAvatar* avatar) const;
    bool isPosingTarget() const { return isPosing(getTarget()); }
    bool startPosing(LLVOAvatar* avatar);
    void stopPosing(LLVOAvatar* avatar);
    // Stops every avatar this session started posing through the studio.
    void stopAll();
    bool isPosedByStudio(const LLUUID& avatar_id) const { return mPosedByStudio.count(avatar_id) != 0; }
    // Self plus owned animesh within range, for the target picker.
    std::vector<LLUUID> getPoseableNearby() const;

    // ---- Bones -----------------------------------------------------------
    const joint_t* findJoint(const std::string& joint_name) const;
    // False when the avatar has no such joint (e.g. no Bento bones on a legacy skeleton).
    bool avatarHasJoint(LLVOAvatar* avatar, const joint_t& joint) const;
    // The bone's dial model (bone_axes.xml): the OP Pose HUD generator's settings for the
    // bones it covers, the twist-axis-X default for every other bone.
    const SLBoneModel& getBoneModel(const std::string& joint_name) const;

    // Rotation of the bone as HUD dial values (degrees). A rotation has several equivalent
    // dial triples (and at the sphere's poles one angle is free), so the studio remembers
    // each bone's dial values: as long as they still produce the bone's rotation they are
    // returned exactly as set; after the rotation changed some other way (load, undo, sync,
    // classic poser) the nearest in-range triple to them is returned and remembered.
    // Mid ease-in blend this is the blend's end rotation, not the in-between frame.
    bool getBoneAngles(LLVOAvatar* avatar, const joint_t& joint, SLPoseAngles& out);
    // Applies the dial values (and the bone's followers, e.g. Ring2/3 with Ring1, the
    // counter-rotated torso with the pelvis); in mirror mode also the mirrored rotation to
    // the opposite bone(s). committed=false while a dial is being dragged. The values are
    // remembered for getBoneAngles.
    bool setBoneAngles(LLVOAvatar* avatar, const joint_t& joint, const SLPoseAngles& angles, bool committed);
    bool setBoneRotation(LLVOAvatar* avatar, const joint_t& joint, const LLQuaternion& rotation, bool committed);
    // Public (spinner) position / scale; mid ease-in blend, the blend's end values.
    LLVector3 getBonePosition(LLVOAvatar* avatar, const joint_t& joint) const;
    // committed=false while a handle is being dragged (local preview only, see notifyEdited).
    void setBonePosition(LLVOAvatar* avatar, const joint_t& joint, const LLVector3& position, bool committed = true);
    LLVector3 getBoneScale(LLVOAvatar* avatar, const joint_t& joint) const;
    void setBoneScale(LLVOAvatar* avatar, const joint_t& joint, const LLVector3& scale);
    // Edited bones: the ones a save writes and pose sharing sends. A bone becomes edited
    // by a dial / spinner / position / scale change (and its mirror partner in mirror mode)
    // or by coming from a loaded pose; Reset Bone removes it, Reset All clears them.
    bool isBoneEdited(const LLUUID& avatar_id, const std::string& joint_name) const;
    // The dial values last set for this bone (no re-derivation from its rotation). Drags use
    // these so turning one axis can never move the other two.
    bool getRememberedAngles(const LLUUID& avatar_id, const std::string& joint_name, SLPoseAngles& out) const;
    bool isBoneChanged(LLVOAvatar* avatar, const joint_t& joint) const;
    void resetBone(LLVOAvatar* avatar, const joint_t& joint);
    void resetAll(LLVOAvatar* avatar);
    void undoBone(LLVOAvatar* avatar, const joint_t& joint);
    void redoBone(LLVOAvatar* avatar, const joint_t& joint);

    // Whole-pose snapshots, for edits that change several bones at once (an IK handle drag)
    // and so need one undo step for all of them: each bone's target transform and whether
    // it is edited. restoreBoneStates puts them back (no ease) and sends a committed edit.
    struct BoneState
    {
        std::string mJoint;
        LLQuaternion mRotation;
        LLVector3 mPosition;
        LLVector3 mScale;
        bool mEdited = false;
    };
    std::vector<BoneState> captureBoneStates(LLVOAvatar* avatar) const;
    void restoreBoneStates(LLVOAvatar* avatar, const std::vector<BoneState>& states);

    // ---- Modes -----------------------------------------------------------
    bool getMirror() const;
    void setMirror(bool mirror);
    // While suppressed, edits do not touch the mirror partner whatever the Mirror setting
    // says (IK holding a pinned chain must not drag the opposite chain along).
    void setMirrorSuppressed(bool suppressed) { mMirrorSuppressed = suppressed; }
    // Freeze on: every bone holds the captured frame. Off: bones you have not edited
    // are released so the animations underneath keep playing on them.
    bool getFreeze() const;
    void setFreeze(bool freeze);
    void applyFreeze(LLVOAvatar* avatar);

    // ---- Pose library ----------------------------------------------------
    static std::string getPoseDirectory();
    static std::string getPosePath(const std::string& pose_name);
    void createUserPoseDirectoryIfNeeded();
    std::vector<std::string> listPoses() const;
    bool poseExists(const std::string& pose_name) const;
    // Saves only the edited bones, with the ease-in time (seconds) stored in the file.
    bool savePose(LLVOAvatar* avatar, const std::string& pose_name, F32 ease_in = DEFAULT_EASE_IN);
    // The ease-in a pose file asks for (DEFAULT_EASE_IN for files without one).
    F32 getPoseEaseIn(const std::string& pose_name) const;
    bool deletePose(const std::string& pose_name);
    // Immediate attempt; returns false if the engine was not ready (see loadPoseRetrying).
    // ease_in < 0: the file's own ease-in. Blends from the current pose over that time.
    bool loadPose(LLVOAvatar* avatar, const std::string& pose_name, E_LoadPoseMethods method = ROT_POS_AND_SCALES, F32 ease_in = -1.f);
    // Loads with a few retries, as diff poses need the captured motions to be ready.
    void loadPoseRetrying(const std::string& pose_name, E_LoadPoseMethods method = ROT_POS_AND_SCALES);
    static S32 tryGetPoseVersion(const std::string& path_to_pose_file);
    bool poseFileStartsFromTeePose(const std::string& pose_name) const;

    // ---- Sequence --------------------------------------------------------
    const std::vector<SequenceEntry>& getSequence() const { return mSequence; }
    void addToSequence(const std::string& pose_name, F32 hold);
    void removeFromSequence(S32 index);
    void moveSequenceEntry(S32 index, S32 delta);
    void setSequenceHold(S32 index, F32 hold);
    void setSequenceEase(S32 index, F32 ease);
    void clearSequence();
    bool getLoop() const;
    void setLoop(bool loop);
    bool isPlaying() const { return mPlaying; }
    S32 getSequenceIndex() const { return mSequenceIndex; }
    void play();
    void stopPlayback();
    void stepSequence(S32 delta);   // previous / next, also while stopped

    // ---- Notifications ---------------------------------------------------
    // Anything the UI should redraw for (target, posing state, library, sequence).
    typedef boost::signals2::signal<void()> changed_signal_t;
    boost::signals2::connection setChangedCallback(const changed_signal_t::slot_type& cb) { return mChangedSignal.connect(cb); }
    // A pose was edited on an avatar: committed=false during a drag (local preview only),
    // true when the edit is final. Pose sync sends on committed edits.
    typedef boost::signals2::signal<void(const LLUUID& avatar_id, bool committed)> edited_signal_t;
    boost::signals2::connection setEditedCallback(const edited_signal_t::slot_type& cb) { return mEditedSignal.connect(cb); }
    // Posing started (true) or stopped (false) on an avatar.
    typedef boost::signals2::signal<void(const LLUUID& avatar_id, bool posing)> posing_signal_t;
    boost::signals2::connection setPosingCallback(const posing_signal_t::slot_type& cb) { return mPosingSignal.connect(cb); }

    void notifyChanged() { mChangedSignal(); }
    // ease: EASE_EDIT for a direct edit, the ease-in time (>= 0, seconds) for a pose load /
    // sequence step, EASE_KEEP to leave the recorded value alone (a blend reaching its pose).
    static constexpr F32 EASE_EDIT = -1.f;
    static constexpr F32 EASE_KEEP = -2.f;
    void notifyEdited(LLVOAvatar* avatar, bool committed, F32 ease = EASE_EDIT);
    // How the last committed change on the avatar came about: the ease-in of a pose load /
    // sequence step (>= 0), or EASE_EDIT for a direct edit (and when nothing is recorded).
    F32 getLastEditEase(const LLUUID& avatar_id) const;

    // ---- Blends ----------------------------------------------------------
    // Ease-in blends, per bone: from where the bone was on screen to the target it holds,
    // smoothstep slerp/lerp. Used by pose loads and by the pose sync's receive smoothing.
    struct BoneBlend
    {
        const joint_t* mJoint = nullptr;
        LLQuaternion mFromRot, mToRot;
        LLVector3 mFromPos, mToPos, mFromScale, mToScale;
        F32 mDuration = 0.f;
        LLTimer mTimer;
    };
    typedef std::map<std::string, BoneBlend> blend_start_t;   // by joint name
    // Where every (non collision volume) bone of the avatar is on screen right now.
    blend_start_t captureBlendStart(LLVOAvatar* avatar) const;
    // The named bones already hold their new targets: ease each from where `from` captured
    // it to that target over duration seconds (<= 0.01 s: they stay where they are set).
    // A named bone that was blending restarts from `from`; other blending bones carry on.
    // notify_when_done: send a committed edit (EASE_KEEP) when the avatar's blend ends.
    void startBlend(LLVOAvatar* avatar, const blend_start_t& from, const std::set<std::string>& bones, F32 duration, bool notify_when_done);
    void cancelBlend(const LLUUID& avatar_id);
    void cancelBoneBlend(const LLUUID& avatar_id, const joint_t& joint);
    // A blending bone's end state, in the same space as getJointTargetRotation /
    // FSPoserAnimator::getJointPosition / getJointScale; false when it is not blending.
    bool getBlendTarget(LLVOAvatar* avatar, const joint_t& joint, LLQuaternion& rotation, LLVector3& position, LLVector3& scale) const;
    // Seconds until the avatar's blend ends; 0 when nothing is blending.
    F32 getBlendRemaining(const LLUUID& avatar_id) const;
    // Every blending bone of the avatar jumps to its end state (an IK drag starts from it).
    void finishBlend(LLVOAvatar* avatar);
    // True while the "blend reached its pose" edit (EASE_KEEP) is being signalled: not a
    // new edit, only the end of an earlier one's ease.
    bool isBlendSettling() const { return mBlendSettling; }

    // ---- Edit ease (SLPoseStudioEditEase) ----------------------------------
    // Seconds a direct edit eases in over (0: at once). A committed edit that was not
    // previewed live (a typed / stepped dial value, a spinner, undo / redo, an IK undo)
    // slerps / lerps each changed bone from where it is on screen to its new target with a
    // smoothstep curve; an edit mid-ease retargets from the eased in-between value. Live
    // drags (dials, IK handles: committed=false previews) follow the mouse directly and
    // their release does not ease again -- it was already watched. Pose sharing sends this
    // time with every direct edit, so viewers that see the pose ease the same way.
    F32 getEditEase() const;
    // A live drag ended without a committed edit (nothing changed): the next edit is not
    // its release.
    void endPreview(const LLUUID& avatar_id) { mPreviewing.erase(avatar_id); }

private:
    bool loadPoseFromFile(LLVOAvatar* avatar, const std::string& pose_name, E_LoadPoseMethods method, std::set<std::string>& loaded);
    void markEdited(LLVOAvatar* avatar, const joint_t& joint, bool with_mirror);
    // Remembers the dial values of a bone whose rotation was set other than through its own
    // dials (followers, mirror partner): decoded nearest to hint (or to what was remembered).
    void rememberRotation(LLVOAvatar* avatar, const joint_t& joint, const LLQuaternion& rotation, const SLPoseAngles* hint);
    void forgetAngles(const LLUUID& avatar_id, const std::string& joint_name);

    // Per avatar, per bone: the dial values last set or shown (see getBoneAngles).
    std::map<LLUUID, std::map<std::string, SLPoseAngles>> mDialAngles;
    void holdOrRelease(LLVOAvatar* avatar, const joint_t& joint);

    // Ease-in blends in progress, per avatar: stepped every frame until each bone arrives.
    struct Blend
    {
        std::map<std::string, BoneBlend> mBones;
        bool mNotify = false;
    };
    void stepBlends();
    static void onIdleBlend(void* data);
    std::map<LLUUID, Blend> mBlends;
    bool mBlendIdleRegistered = false;
    bool mBlendSettling = false;

    // Edit ease (see getEditEase): beginEdit before a direct edit's bones are set,
    // endEdit after; nested calls (setBoneAngles -> setBoneRotation) count as one edit.
    void beginEdit(LLVOAvatar* avatar, bool committed);
    void endEdit(LLVOAvatar* avatar);
    S32 mEditDepth = 0;
    bool mEditEasing = false;
    blend_start_t mEditFrom;                // what was on screen
    std::vector<BoneState> mEditBefore;     // what every bone held
    std::set<LLUUID> mPreviewing;           // a live drag is under way (uncommitted edits)
    S32 mEditedSignalDepth = 0;             // inside mEditedSignal (listeners re-editing)
    std::map<LLUUID, F32> mEditEase;   // see getLastEditEase
    std::map<LLUUID, std::set<std::string>> mEdited;
    void ensureTicker();
    void onTick();
    void loadSequence();
    void saveSequence() const;
    void startSequenceTimer(F32 seconds);

    FSPoserAnimator mAnimator;
    bool mMirrorSuppressed = false;
    LLUUID mTargetID;
    std::set<LLUUID> mPosedByStudio;
    void loadBoneModels();
    SLBoneModel mDefaultModel;
    std::map<std::string, SLBoneModel> mBoneModels;

    std::vector<SequenceEntry> mSequence;
    bool mPlaying = false;
    S32 mSequenceIndex = -1;
    F32 mSequenceHold = 0.f;
    LLTimer mSequenceTimer;

    std::string mRetryPose;
    E_LoadPoseMethods mRetryMethod = ROT_POS_AND_SCALES;
    S32 mRetryAttempts = 0;
    LLTimer mRetryTimer;

    LLEventTimer* mTicker = nullptr;

    changed_signal_t mChangedSignal;
    edited_signal_t mEditedSignal;
    posing_signal_t mPosingSignal;
};

#endif // SL_POSESTUDIO_H
