/**
 * @file slposesync.h
 * @brief Pose Studio (#500): share poses with other Slipstream users through the
 *        #Slipstream Helper bridge, and pose other people with their consent.
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

#ifndef SL_POSESYNC_H
#define SL_POSESYNC_H

#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <boost/signals2.hpp>

#include "llquaternion.h"
#include "llsingleton.h"
#include "lltimer.h"
#include "lluuid.h"
#include "v3math.h"

class LLEventTimer;
class LLVOAvatar;

/**
 * Wire protocol, version 1. Pose lines are chat on SLBridge::POSE_CHANNEL, said by the
 * sender's bridge with llRegionSay; consent lines are on SLBridge::CONSENT_CHANNEL.
 * Receiving bridges prefix the sim-verified identity of the speaker (owner of the
 * speaking attachment, or the avatar for a dialog answer) before handing a line to
 * their viewer, so no identity is ever taken from a message body.
 *
 * Pose channel:
 *   P1|<seq>|<kind>|<part>/<total>|<target>|<data>     pose data
 *      kind   F = full pose (replaces everything), D = delta (changed bones only),
 *             X = stop (no longer posed; data empty)
 *             L = look-at (#902): where the sender's own avatar looks; target always "-",
 *                 one part; format in slposelookat.h. Not a pose: no remote, no Q1.
 *      seq    0..65535, per sender; all parts of one full share its seq
 *      target "-" = the sender's own avatar, else the posed avatar's key -- only honoured
 *             with a verified consent grant from that avatar to the sender
 *      data   base64url of bone records:
 *               U8 bone index (FSPoserAnimator::PoserJoints order, protocol v1)
 *               U8 flags: 1 rotation, 2 position, 4 scale, 8 released (not posed)
 *               rotation: 6 bytes, smallest-three quaternion (SLPoseQuant)
 *               position / scale: 3 x S16 big-endian, millimetres / thousandths
 *             index 255 is never a bone (there are ~130): it marks a meta record, always
 *             sent with flags 2 so that every receiver, old or new, knows it is 6 bytes
 *             long; older viewers skip it as an unknown bone. Payload: U8 type, 5 bytes.
 *               type 'E' (#531): ease -- U16 big-endian milliseconds, 3 zero bytes. The
 *               line comes from a pose load / sequence step, or (#530, same record, no
 *               format change) from a direct edit -- dial, IK handle, undo -- and then
 *               carries the poser's edit ease (SLPoseStudioEditEase): receivers blend into
 *               it over this time (0 = at once). Without it (older senders) they apply
 *               their short receive smoothing (SLPoseSyncReceiveSmoothMs). Sent first, so
 *               in part 1 of a full.
 *   Q1|<key or *>      please send your full pose(s) (newcomer, delta without baseline)
 *   R1|<target>        the sender asked <target> for consent (pairs with the answer)
 *   E1|<target>        the sender stopped posing <target>: the grant ends
 *
 * Consent channel (the speaker is the person consenting):
 *   Accept <tag> / Deny <tag> / Stop <tag>   dialog answers; <tag> = first 8 hex digits
 *                                            of the poser's key, matched to a recent R1
 *   G1|<poser> / S1|<poser>                  consent v1, said by a target's bridge
 *                                            (an attachment) region-wide. Any attachment of
 *                                            the target can say this, so since consent v2 a
 *                                            G1 here grants nothing (S1 still revokes). Still
 *                                            sent, for viewers that predate v2.
 * Confirm channel (SLBridge::CONFIRM_CHANNEL, positive; consent v2):
 *   G1|<poser> / S1|<poser>                  renew / revoke, said by the target's AVATAR
 *                                            (its viewer chats, shout range 100 m). Only an
 *                                            avatar speaker counts, which no attachment can
 *                                            imitate. A Slipstream target renews every
 *                                            RENEW_INTERVAL; grants not renewed for twice that
 *                                            expire. A G1 attributed to ourselves by one of
 *                                            our attachments is answered with S1 at once.
 *
 * Hearing range: a dialog answer is plain chat said by the answering avatar, so only
 * bridges within chat range (20 m) of the target hear it. Slipstream targets re-state the
 * grant region-wide (G1); for a target on another viewer, bystanders further away than
 * that cannot verify the grant and never show the pose. The target's own Slipstream
 * viewer applies its answer locally (callback_script_dialog), whatever its bridge heard.
 *
 * Rates: deltas on dial release, at most one per DELTA_INTERVAL per posed avatar; a full
 * pose on start, every FULL_INTERVAL and on request (at most every REQUEST_COOLDOWN).
 * Lines are capped at MAX_LINE_BYTES so the receiving bridge's prefix still fits
 * llOwnerSay's 1024 bytes.
 */
class SLPoseSync : public LLSingleton<SLPoseSync>
{
    LLSINGLETON(SLPoseSync);
    ~SLPoseSync() override;
    LOG_CLASS(SLPoseSync);

public:
    static constexpr size_t MAX_LINE_BYTES = 900;

    // After login: hooks the bridge and the studio, applies the saved settings.
    void init();

    bool isSharing() const;
    void setSharing(bool share);
    bool isShowingOthers() const;
    void setShowingOthers(bool show);

    // Avatars currently shown with a synced pose.
    std::vector<LLUUID> getSyncedAvatars() const;
    bool isIgnored(const LLUUID& avatar_id) const;
    std::vector<LLUUID> getIgnoredList() const { return std::vector<LLUUID>(mIgnored.begin(), mIgnored.end()); }
    void setIgnored(const LLUUID& avatar_id, bool ignored);

    // ---- Consent (posing other people) -----------------------------------
    // A grant this viewer verified itself: target answered (or its own bridge said) yes
    // to poser, and neither has stopped it, left, or teleported.
    bool hasVerifiedGrant(const LLUUID& target, const LLUUID& poser) const;
    // Ask someone nearby for consent. False (with a reason) if it cannot be sent now.
    bool requestPose(const LLUUID& target, std::string& reason);
    // We stop posing target (or our grant was revoked): tell everyone.
    void endPosing(const LLUUID& target);
    // People posing us right now (verified), and the control to stop all of them.
    std::vector<LLUUID> getPosers() const;
    void stopBeingPosed();
    // Pose-request blocking.
    bool isRequestBlocked(const LLUUID& poser) const;
    std::vector<LLUUID> getBlockedRequesters() const { return std::vector<LLUUID>(mBlockedRequesters.begin(), mBlockedRequesters.end()); }
    void setRequestBlocked(const LLUUID& poser, bool blocked);
    // From process_script_dialog for a dialog on the consent channel. Returns false
    // when the dialog must not be shown (blocked, too many requests).
    bool onConsentDialog(const LLUUID& owner_id, const std::string& first_button);
    // From callback_script_dialog: our own answer to a consent-channel dialog from
    // owner_id's object, applied locally (our bridge may not be listening yet).
    void onOwnConsentAnswer(const LLUUID& owner_id, const std::string& button);

    typedef boost::signals2::signal<void()> changed_signal_t;
    boost::signals2::connection setChangedCallback(const changed_signal_t::slot_type& cb) { return mChangedSignal.connect(cb); }

private:
    struct BoneState
    {
        bool mPosed = false;
        LLQuaternion mRotation;
        LLVector3 mPosition;
        LLVector3 mScale;
    };
    typedef std::map<U8, BoneState> pose_t;

    // One avatar we send poses for: ourselves while sharing, or a consenting target.
    struct Outgoing
    {
        pose_t mLastSent;
        bool mAnnounced = false;
        bool mDeltaPending = false;
        bool mFullPending = false;
        F32 mPendingEase = -1.f;    // ease of the load / edit not sent yet; < 0 = none
        LLTimer mLastDelta;
        LLTimer mLastFull;
    };

    struct Remote
    {
        LLUUID mSender;
        LLUUID mTarget;
        bool mHasBaseline = false;
        U16 mFullSeq = 0;
        S32 mPartsExpected = 0;
        std::map<S32, std::string> mParts;  // part number -> decoded data, for the full being assembled
        LLTimer mLastHeard;
        LLTimer mLastRequest;
        bool mRequested = false;
    };

    typedef std::pair<LLUUID, LLUUID> pair_t;   // (poser, target)

    void updateBridgeUse();
    void tick();
    void ensureTicker();

    // Sending
    pose_t capturePose(LLVOAvatar* avatar) const;
    F32 takeOutgoingEase(Outgoing& out, const LLUUID& id);
    void sendFull(LLVOAvatar* avatar, Outgoing& out, const std::string& target);
    void sendDelta(LLVOAvatar* avatar, Outgoing& out, const std::string& target);
    void sendStop(const std::string& target);
    void sendRecords(const std::vector<std::string>& records, char kind, U16 seq, const std::string& target);
    void tickOutgoing(LLVOAvatar* avatar, bool active, const LLUUID& id);
    void onPoseEdited(const LLUUID& avatar_id, bool committed);
    void onPosingChanged(const LLUUID& avatar_id, bool posing);

    // Receiving
    void onBridgePose(const std::string& rest);
    void onBridgeConsent(const std::string& rest);
    void handlePoseLine(const LLUUID& sender, const std::vector<std::string>& fields);
    void applyRecords(Remote& remote, const std::string& data, bool full);
    void dropRemote(const std::string& key, bool stop_posing);
    void dropRemotesFor(const LLUUID& sender, const LLUUID& target);
    void requestFull(Remote& remote);
    LLVOAvatar* findAvatar(const LLUUID& id) const;
    // Any region: for stopping a pose on an avatar that has moved on.
    LLVOAvatar* findAnyAvatar(const LLUUID& id) const;
    static std::string remoteKey(const LLUUID& sender, const LLUUID& target);

    // Consent
    void addGrant(const LLUUID& target, const LLUUID& poser);
    void revokeGrant(const LLUUID& target, const LLUUID& poser);
    void clearConsent();
    void onRegionChanged();
    void onBridgeReadyInRegion();
    void onBridgeError(const std::string& rest);
    void sayGrant(const LLUUID& poser);
    void sayRevoke(const LLUUID& poser);
    void onForgedGrant(const LLUUID& object_id, const std::string& poser);
    bool notifyAllowed(const LLUUID& who);
    void pruneConsent();
    static std::string tagFor(const LLUUID& poser);

    static std::string encodeBone(U8 index, const BoneState& bone);
    static std::string encodeEase(F32 seconds);
    static std::vector<std::string> split(const std::string& text, char sep, size_t max_fields);

    // Sender state
    std::map<LLUUID, Outgoing> mOutgoing;   // gAgentID for our own avatar
    U16 mSeq = 0;
    LLTimer mLastFullOnRequest;

    // Receiver state, keyed by sender|target
    std::map<std::string, Remote> mRemotes;
    std::map<LLUUID, LLTimer> mFullRequestsFrom;    // Q1 handling, per requester
    std::set<LLUUID> mIgnored;
    LLTimer mLastBroadcastRequest;
    bool mBroadcastRequested = false;

    // Consent state
    std::map<pair_t, LLTimer> mPendingRequests;     // R1 heard: poser asked target
    std::set<pair_t> mGrants;                       // verified (poser, target)
    std::map<pair_t, LLTimer> mGrantRenewals;       // last G1 per grant (Slipstream targets)
    std::map<LLUUID, LLTimer> mNotified;            // consent toasts, per person
    std::map<LLUUID, LLTimer> mAsked;               // our own requests, per target
    std::map<LLUUID, LLTimer> mDialogsFrom;         // incoming request dialogs, per poser
    std::set<LLUUID> mBlockedRequesters;
    LLTimer mLastRenewal;
    LLTimer mStatsTimer;
    LLTimer mForgedNotify;
    std::map<LLUUID, LLTimer> mForgedRevoked;   // forged-grant revocations sent, per poser
    bool mForgedNotified = false;  // a forged-grant toast was shown (then rate-limited)
    LLTimer mWarmup;            // a request found the helper not ready yet: keep it starting
    bool mWarmingUp = false;

    bool mInitialized = false;
    LLEventTimer* mTicker = nullptr;
    changed_signal_t mChangedSignal;
    boost::signals2::scoped_connection mEditedConnection;
    boost::signals2::scoped_connection mPosingConnection;
    boost::signals2::scoped_connection mBridgeConnection;
    boost::signals2::scoped_connection mRegionConnection;
    boost::signals2::scoped_connection mReadyInRegionConnection;
};

#endif // SL_POSESYNC_H
