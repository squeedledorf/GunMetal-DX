/**
 * @file slposesync.cpp
 * @brief Pose Studio (#500): share local poses with other Slipstream users.
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

#include "slposesync.h"

#include "llagent.h"
#include "lleventtimer.h"
#include "llmutelist.h"
#include "llnotificationsutil.h"
#include "llslurl.h"
#include "llviewercontrol.h"
#include "llviewerinventory.h"
#include "llinventorymodel.h"
#include "llviewerobjectlist.h"
#include "llvoavatarself.h"
#include "rlvactions.h"
#include "slbridge.h"
#include "slposelookat.h"
#include "slposemath.h"
#include "slposestudio.h"

#include <cmath>

namespace
{
constexpr char BRIDGE_USER[]        = "posesync";
constexpr char CONSENT_BRIDGE_USER[] = "poseconsent";
constexpr F32  TICK_PERIOD          = 0.25f;
constexpr F32  DELTA_INTERVAL       = 0.5f;     // at most two deltas a second
constexpr F32  FULL_INTERVAL        = 30.f;     // periodic full pose for newcomers / lost lines
constexpr F32  REQUEST_COOLDOWN     = 5.f;      // answer full-pose requests at most this often
constexpr F32  REMOTE_REQUEST_COOLDOWN = 15.f;  // ask one sender for a full at most this often
constexpr F32  BROADCAST_REQUEST_COOLDOWN = 30.f;
constexpr F32  REMOTE_EXPIRY        = 75.f;     // two missed periodic fulls: the sender is gone
constexpr size_t MAX_DATA_BYTES     = 600;      // per line, before base64 (800 characters)
constexpr S32  MAX_PARTS            = 16;
constexpr F32  PENDING_EXPIRY       = 120.f;    // an unanswered request is forgotten after this
constexpr F32  ASK_COOLDOWN         = 60.f;     // we ask one person at most this often
constexpr F32  DIALOG_COOLDOWN      = 60.f;     // we show one person's requests at most this often
constexpr F32  RENEW_INTERVAL       = 45.f;     // a Slipstream target re-states its grants region-wide
constexpr F32  CONSENT_ACTIVE_WINDOW = 120.f;   // keep the bridge on this long around a request
constexpr F32  GRANT_RENEW_EXPIRY   = 2.f * RENEW_INTERVAL;  // a Slipstream target's grant without G1
constexpr F32  NOTIFY_COOLDOWN      = 30.f;     // consent toasts about one person
constexpr size_t MAX_PENDING        = 256;      // R1 requests remembered
constexpr size_t MAX_PENDING_PER_POSER = 8;
constexpr size_t MAX_GRANTS         = 256;
constexpr size_t MAX_REMOTES        = 64;
constexpr size_t MAX_LIST_ENTRIES   = 500;      // ignored / blocked people kept
constexpr F32  STATS_INTERVAL       = 60.f;
constexpr size_t MAX_DATA_CHARS     = (MAX_DATA_BYTES * 4 + 2) / 3;    // base64url of MAX_DATA_BYTES
constexpr size_t MAX_RATE_ENTRIES   = 256;      // per-person timers (Q1 answers, dialogs)
constexpr F32  FORGED_NOTIFY_COOLDOWN = 60.f;
constexpr F32  FORGED_REVOKE_COOLDOWN = 30.f;   // one revocation per forged poser this often
constexpr size_t MAX_FORGED_TRACKED = 64;
constexpr F32  ROTATION_EPSILON     = 0.0015f;  // radians (~0.1 degree): smaller changes are not sent
constexpr F32  VECTOR_EPSILON       = 0.0005f;

constexpr U8 FLAG_ROTATION = 1;
constexpr U8 FLAG_POSITION = 2;
constexpr U8 FLAG_SCALE    = 4;
constexpr U8 FLAG_RELEASED = 8;

// Meta records (see the protocol comment in slposesync.h): index 255, flags FLAG_POSITION
// so older parsers skip exactly META_PAYLOAD bytes, then a type byte.
constexpr U8 META_INDEX    = 255;
constexpr S32 META_PAYLOAD = 6;
constexpr U8 META_EASE     = 'E';
constexpr U32 MAX_EASE_MS  = 60000;     // well above any studio ease; U16 on the wire

class SLPoseSyncTicker : public LLEventTimer
{
public:
    SLPoseSyncTicker(F32 period, std::function<void()> cb) : LLEventTimer(period), mCallback(std::move(cb)) {}
    bool tick() override
    {
        if (mCallback)
            mCallback();
        return false;
    }

private:
    std::function<void()> mCallback;
};

void putS16(std::string& out, F32 value)
{
    const S32 v = llclamp(ll_round(value * 1000.f), -32767, 32767);
    const U16 u = (U16)(S16)v;
    out += (char)(u >> 8);
    out += (char)(u & 0xFF);
}

F32 getS16(const U8* p)
{
    const S16 v = (S16)(((U16)p[0] << 8) | (U16)p[1]);
    return (F32)v / 1000.f;
}

bool rotationsDiffer(const LLQuaternion& a, const LLQuaternion& b)
{
    F32 dot = llabs(a.mQ[VX] * b.mQ[VX] + a.mQ[VY] * b.mQ[VY] + a.mQ[VZ] * b.mQ[VZ] + a.mQ[VW] * b.mQ[VW]);
    dot = llmin(dot, 1.f);
    return 2.f * acosf(dot) > ROTATION_EPSILON;
}

bool vectorsDiffer(const LLVector3& a, const LLVector3& b)
{
    return (a - b).lengthSquared() > VECTOR_EPSILON * VECTOR_EPSILON;
}

bool isSyncedJoint(const FSPoserAnimator::FSPoserJoint& joint)
{
    // Collision volumes only carry physics/shape tweaks; they are not synced.
    return joint.boneType() != COL_VOLUMES;
}
}

SLPoseSync::SLPoseSync()
{
}

SLPoseSync::~SLPoseSync()
{
    delete mTicker;
    mTicker = nullptr;
}

void SLPoseSync::init()
{
    if (mInitialized)
        return;
    mInitialized = true;

    mIgnored.clear();
    std::vector<std::string> ids;
    LLStringUtil::getTokens(gSavedSettings.getString("SLPoseSyncIgnored"), ids, ",");
    for (const std::string& id : ids)
    {
        LLUUID uuid;
        if (LLUUID::parseUUID(id, &uuid))
            mIgnored.insert(uuid);
    }

    mBlockedRequesters.clear();
    ids.clear();
    LLStringUtil::getTokens(gSavedSettings.getString("SLPoseSyncBlockedRequesters"), ids, ",");
    for (const std::string& id : ids)
    {
        LLUUID uuid;
        if (LLUUID::parseUUID(id, &uuid))
            mBlockedRequesters.insert(uuid);
    }

    SLBridge& bridge = SLBridge::instance();
    bridge.init();
    bridge.registerHandler("pose", [this](const std::string& rest) { onBridgePose(rest); });
    bridge.registerHandler("consent", [this](const std::string& rest) { onBridgeConsent(rest); });
    bridge.registerHandler("err", [this](const std::string& rest) { onBridgeError(rest); });
    mReadyInRegionConnection = bridge.setReadyInRegionCallback([this]() { onBridgeReadyInRegion(); });
    mRegionConnection = gAgent.addRegionChangedCallback([this]() { onRegionChanged(); });
    mBridgeConnection = bridge.setStateCallback([this](SLBridge::EState state)
        {
            if (state == SLBridge::STATE_READY)
            {
                // Fresh bridge: announce our poses again and ask sharers nearby for theirs.
                for (auto& entry : mOutgoing)
                    entry.second.mAnnounced = false;
                if (isShowingOthers() && (!mBroadcastRequested || mLastBroadcastRequest.getElapsedTimeF32() > BROADCAST_REQUEST_COOLDOWN))
                {
                    mBroadcastRequested = true;
                    mLastBroadcastRequest.reset();
                    SLBridge::instance().sendControl("say|Q1|*");
                }
            }
            mChangedSignal();
        });

    SLPoseStudio& studio = SLPoseStudio::instance();
    mEditedConnection = studio.setEditedCallback([this](const LLUUID& id, bool committed) { onPoseEdited(id, committed); });
    mPosingConnection = studio.setPosingCallback([this](const LLUUID& id, bool posing) { onPosingChanged(id, posing); });

    ensureTicker();
    updateBridgeUse();
}

void SLPoseSync::ensureTicker()
{
    if (!mTicker)
        mTicker = new SLPoseSyncTicker(TICK_PERIOD, [this]() { tick(); });
}

bool SLPoseSync::isSharing() const
{
    static LLCachedControl<bool> share(gSavedSettings, "SLPoseSyncShare", true);
    return share;
}

void SLPoseSync::setSharing(bool share)
{
    gSavedSettings.setBOOL("SLPoseSyncShare", share);
    if (share)
        mOutgoing[gAgentID].mAnnounced = false;
    updateBridgeUse();
    mChangedSignal();
}

bool SLPoseSync::isShowingOthers() const
{
    static LLCachedControl<bool> show(gSavedSettings, "SLPoseSyncShowOthers", true);
    return show;
}

void SLPoseSync::setShowingOthers(bool show)
{
    gSavedSettings.setBOOL("SLPoseSyncShowOthers", show);
    if (!show)
    {
        std::vector<std::string> keys;
        for (const auto& entry : mRemotes)
            keys.push_back(entry.first);
        for (const std::string& key : keys)
            dropRemote(key, true);
    }
    updateBridgeUse();
    mChangedSignal();
}

void SLPoseSync::updateBridgeUse()
{
    SLBridge& bridge = SLBridge::instance();
    if (isSharing() || isShowingOthers())
        bridge.addUser(BRIDGE_USER);
    else
        bridge.removeUser(BRIDGE_USER);

    // Consent needs the bridge while a request is out, around an incoming request
    // dialog (so our own "Accept" is heard and we see ourselves posed), and while any
    // grant involving us is active.
    bool consent = false;
    for (const pair_t& grant : mGrants)
        consent |= (grant.first == gAgentID || grant.second == gAgentID);
    for (const auto& [id, timer] : mAsked)
        consent |= timer.getElapsedTimeF32() < CONSENT_ACTIVE_WINDOW;
    for (const auto& [id, timer] : mDialogsFrom)
        consent |= timer.getElapsedTimeF32() < CONSENT_ACTIVE_WINDOW;
    consent |= mWarmingUp && mWarmup.getElapsedTimeF32() < CONSENT_ACTIVE_WINDOW;
    if (consent)
        bridge.addUser(CONSENT_BRIDGE_USER);
    else
        bridge.removeUser(CONSENT_BRIDGE_USER);
}

std::vector<LLUUID> SLPoseSync::getSyncedAvatars() const
{
    std::vector<LLUUID> ids;
    for (const auto& entry : mRemotes)
    {
        if (entry.second.mHasBaseline)
            ids.push_back(entry.second.mTarget);
    }
    return ids;
}

bool SLPoseSync::isIgnored(const LLUUID& avatar_id) const
{
    return mIgnored.count(avatar_id) != 0;
}

void SLPoseSync::setIgnored(const LLUUID& avatar_id, bool ignored)
{
    if (ignored)
    {
        if (mIgnored.size() >= MAX_LIST_ENTRIES && !mIgnored.count(avatar_id))
        {
            LL_WARNS("PoseSync") << "Ignore list is full (" << MAX_LIST_ENTRIES << ")" << LL_ENDL;
            return;
        }
        mIgnored.insert(avatar_id);
        std::vector<std::string> drop;
        for (const auto& entry : mRemotes)
        {
            if (entry.second.mSender == avatar_id || entry.second.mTarget == avatar_id)
                drop.push_back(entry.first);
        }
        for (const std::string& key : drop)
            dropRemote(key, true);
    }
    else
    {
        mIgnored.erase(avatar_id);
    }

    std::string list;
    for (const LLUUID& id : mIgnored)
        list += (list.empty() ? "" : ",") + id.asString();
    gSavedSettings.setString("SLPoseSyncIgnored", list);
    mChangedSignal();
}

// ---------------------------------------------------------------------------
// Sending
// ---------------------------------------------------------------------------

SLPoseSync::pose_t SLPoseSync::capturePose(LLVOAvatar* avatar) const
{
    pose_t pose;
    FSPoserAnimator& animator = SLPoseStudio::instance().getAnimator();
    if (!avatar || !animator.isPosingAvatar(avatar))
        return pose;

    const auto& joints = animator.PoserJoints;
    for (size_t i = 0; i < joints.size() && i < 256; ++i)
    {
        if (!isSyncedJoint(joints[i]))
            continue;
        // Only edited bones are part of the pose; everything else is "released" so the
        // receiver plays the avatar's own animation on it (frozen bones are not shared).
        BoneState bone;
        bone.mPosed = SLPoseStudio::instance().isBoneEdited(avatar->getID(), joints[i].jointName()) &&
                      animator.isPosingAvatarJoint(avatar, joints[i]);
        if (bone.mPosed)
        {
            if (!animator.getJointTargetRotation(avatar, joints[i], bone.mRotation))
                continue;
            bone.mPosition = animator.getJointPosition(avatar, joints[i]);
            bone.mScale = animator.getJointScale(avatar, joints[i]);
            // Still easing into a loaded pose: send where the bone is going, so receivers
            // ease to the pose itself (and the blend's end needs no second line).
            SLPoseStudio::instance().getBlendTarget(avatar, joints[i], bone.mRotation, bone.mPosition, bone.mScale);
        }
        pose[(U8)i] = bone;
    }
    return pose;
}

// static
std::string SLPoseSync::encodeBone(U8 index, const BoneState& bone)
{
    std::string out;
    out += (char)index;
    if (!bone.mPosed)
    {
        out += (char)FLAG_RELEASED;
        return out;
    }

    U8 flags = FLAG_ROTATION;
    const bool has_pos = bone.mPosition.lengthSquared() > VECTOR_EPSILON * VECTOR_EPSILON;
    const bool has_scale = bone.mScale.lengthSquared() > VECTOR_EPSILON * VECTOR_EPSILON;
    if (has_pos)
        flags |= FLAG_POSITION;
    if (has_scale)
        flags |= FLAG_SCALE;
    out += (char)flags;

    U8 packed[6];
    SLPoseQuant::packQuat(bone.mRotation, packed);
    out.append((const char*)packed, 6);
    if (has_pos)
    {
        for (S32 i = 0; i < 3; ++i)
            putS16(out, bone.mPosition.mV[i]);
    }
    if (has_scale)
    {
        for (S32 i = 0; i < 3; ++i)
            putS16(out, bone.mScale.mV[i]);
    }
    return out;
}

// static
std::string SLPoseSync::encodeEase(F32 seconds)
{
    const U32 ms = (U32)llclamp(ll_round(seconds * 1000.f), 0, (S32)MAX_EASE_MS);
    std::string out;
    out += (char)META_INDEX;
    out += (char)FLAG_POSITION;
    out += (char)META_EASE;
    out += (char)(ms >> 8);
    out += (char)(ms & 0xFF);
    out.append(META_PAYLOAD - 3, '\0');
    return out;
}

F32 SLPoseSync::takeOutgoingEase(Outgoing& out, const LLUUID& id)
{
    // While the studio is still easing into a loaded pose, the time it has left (receivers
    // arrive with us, also for a periodic full or a Q1 answer mid-blend); otherwise the
    // ease of a load that has not gone out yet. < 0: a plain edit, no ease sent.
    F32 ease = SLPoseStudio::instance().getBlendRemaining(id);
    if (ease <= 0.f)
        ease = out.mPendingEase;
    out.mPendingEase = -1.f;
    return ease;
}

void SLPoseSync::sendRecords(const std::vector<std::string>& records, char kind, U16 seq, const std::string& target)
{
    // Pack whole records into lines; a record never straddles two lines.
    std::vector<std::string> chunks(1);
    for (const std::string& record : records)
    {
        if (chunks.back().size() + record.size() > MAX_DATA_BYTES)
            chunks.emplace_back();
        chunks.back() += record;
    }
    if ((S32)chunks.size() > MAX_PARTS)
    {
        LL_WARNS("PoseSync") << "Pose too large to send (" << chunks.size() << " lines)" << LL_ENDL;
        return;
    }

    const S32 total = (S32)chunks.size();
    for (S32 i = 0; i < total; ++i)
    {
        std::string line = llformat("P1|%u|%c|%d/%d|%s|", (U32)seq, kind, i + 1, total, target.c_str());
        line += SLPoseQuant::encode64((const U8*)chunks[i].data(), chunks[i].size());
        if (line.size() > MAX_LINE_BYTES)
        {
            LL_WARNS("PoseSync") << "Pose line over budget, not sent" << LL_ENDL;
            return;
        }
        SLBridge::instance().sendCommand("say|" + line);
    }
}

void SLPoseSync::sendFull(LLVOAvatar* avatar, Outgoing& out, const std::string& target)
{
    const pose_t pose = capturePose(avatar);
    std::vector<std::string> records;
    const F32 ease = takeOutgoingEase(out, avatar->getID());
    if (ease >= 0.f)
        records.push_back(encodeEase(ease));
    for (const auto& [index, bone] : pose)
    {
        if (bone.mPosed)
        {
            records.push_back(encodeBone(index, bone));
        }
        else
        {
            // A full pose implicitly releases every bone it does not name; bones we sent
            // as posed before are also released explicitly.
            auto it = out.mLastSent.find(index);
            if (it != out.mLastSent.end() && it->second.mPosed)
                records.push_back(encodeBone(index, bone));
        }
    }
    sendRecords(records, 'F', ++mSeq, target);
    out.mLastSent = pose;
    out.mLastFull.reset();
    out.mLastDelta.reset();
}

void SLPoseSync::sendDelta(LLVOAvatar* avatar, Outgoing& out, const std::string& target)
{
    const pose_t pose = capturePose(avatar);
    std::vector<std::string> records;
    for (const auto& [index, bone] : pose)
    {
        auto it = out.mLastSent.find(index);
        bool changed = (it == out.mLastSent.end()) || (it->second.mPosed != bone.mPosed);
        if (!changed && bone.mPosed)
        {
            changed = rotationsDiffer(it->second.mRotation, bone.mRotation) || vectorsDiffer(it->second.mPosition, bone.mPosition) ||
                      vectorsDiffer(it->second.mScale, bone.mScale);
        }
        if (changed)
            records.push_back(encodeBone(index, bone));
    }
    const F32 ease = takeOutgoingEase(out, avatar->getID());
    if (!records.empty())
    {
        if (ease >= 0.f)
            records.insert(records.begin(), encodeEase(ease));
        sendRecords(records, 'D', ++mSeq, target);
    }
    out.mLastSent = pose;
    out.mLastDelta.reset();
}

void SLPoseSync::sendStop(const std::string& target)
{
    // A stop must not be lost to a full data queue or a re-handshake.
    SLBridge::instance().sendControl(llformat("say|P1|%u|X|1/1|%s|", (U32)++mSeq, target.c_str()));
}

void SLPoseSync::tickOutgoing(LLVOAvatar* avatar, bool active, const LLUUID& id)
{
    Outgoing& out = mOutgoing[id];
    const std::string target = (id == gAgentID) ? std::string("-") : id.asString();
    if (active && avatar)
    {
        if (!out.mAnnounced || out.mFullPending || out.mLastFull.getElapsedTimeF32() > FULL_INTERVAL)
        {
            sendFull(avatar, out, target);
            out.mAnnounced = true;
            out.mFullPending = false;
            out.mDeltaPending = false;
        }
        else if (out.mDeltaPending && out.mLastDelta.getElapsedTimeF32() > DELTA_INTERVAL)
        {
            sendDelta(avatar, out, target);
            out.mDeltaPending = false;
        }
    }
    else if (out.mAnnounced)
    {
        sendStop(target);
        out.mAnnounced = false;
        out.mLastSent.clear();
    }
}

void SLPoseSync::onPoseEdited(const LLUUID& avatar_id, bool committed)
{
    // Preview locally while dragging; only released edits go out.
    if (!committed)
        return;
    if ((avatar_id == gAgentID && isSharing()) || hasVerifiedGrant(avatar_id, gAgentID))
    {
        // A pose load / sequence step carries its ease-in; a direct edit (dial, IK, undo)
        // the studio's edit ease (SLPoseStudioEditEase, 0 = at once), so viewers that see
        // the pose ease into the change the way the poser sees it.
        Outgoing& out = mOutgoing[avatar_id];
        out.mDeltaPending = true;
        out.mPendingEase = SLPoseStudio::instance().getLastEditEase(avatar_id);
        if (out.mPendingEase < 0.f)
            out.mPendingEase = SLPoseStudio::instance().getEditEase();
    }
}

void SLPoseSync::onPosingChanged(const LLUUID& avatar_id, bool posing)
{
    if (avatar_id == gAgentID)
    {
        if (isSharing())
            mOutgoing[gAgentID].mFullPending = posing;
        return;
    }
    if (posing && hasVerifiedGrant(avatar_id, gAgentID))
        mOutgoing[avatar_id].mFullPending = true;
    else if (!posing)
        endPosing(avatar_id);   // the poser stopping ends the grant
}

// ---------------------------------------------------------------------------
// Receiving
// ---------------------------------------------------------------------------

// static
std::vector<std::string> SLPoseSync::split(const std::string& text, char sep, size_t max_fields)
{
    // Keeps empty fields; the last field takes the rest of the line.
    std::vector<std::string> fields;
    std::string::size_type start = 0;
    while (fields.size() + 1 < max_fields)
    {
        const std::string::size_type at = text.find(sep, start);
        if (at == std::string::npos)
            break;
        fields.push_back(text.substr(start, at - start));
        start = at + 1;
    }
    fields.push_back(text.substr(start));
    return fields;
}

// static
std::string SLPoseSync::remoteKey(const LLUUID& sender, const LLUUID& target)
{
    return sender.asString() + "|" + target.asString();
}

LLVOAvatar* SLPoseSync::findAvatar(const LLUUID& id) const
{
    LLViewerObject* object = gObjectList.findObject(id);
    if (!object || object->isDead() || !object->isAvatar())
        return nullptr;
    LLVOAvatar* avatar = static_cast<LLVOAvatar*>(object);
    // The posing engine only drives avatars in our region.
    return (avatar->getRegion() == gAgent.getRegion()) ? avatar : nullptr;
}

LLVOAvatar* SLPoseSync::findAnyAvatar(const LLUUID& id) const
{
    LLViewerObject* object = gObjectList.findObject(id);
    if (!object || object->isDead() || !object->isAvatar())
        return nullptr;
    return static_cast<LLVOAvatar*>(object);
}

void SLPoseSync::onBridgePose(const std::string& rest)
{
    // rest = <sim-verified owner of the speaking attachment>|<line as said>
    const std::vector<std::string> outer = split(rest, '|', 2);
    if (outer.size() != 2)
        return;
    LLUUID sender;
    if (!LLUUID::parseUUID(outer[0], &sender) || sender.isNull() || sender == gAgentID)
        return;
    const std::string& line = outer[1];

    if (line.compare(0, 3, "Q1|") == 0)
    {
        // Answer each requester at most once per FULL_INTERVAL (and everyone together at
        // most every REQUEST_COOLDOWN), so one noisy attachment cannot make every sharer
        // in the region resend full poses all the time. Muted people are not answered.
        const std::string who = line.substr(3);
        if (who != "*" && who != gAgentID.asString())
            return;
        if (LLMuteList::getInstance()->isMuted(sender))
            return;
        auto from = mFullRequestsFrom.find(sender);
        if (from != mFullRequestsFrom.end() && from->second.getElapsedTimeF32() < FULL_INTERVAL)
            return;
        if (from == mFullRequestsFrom.end() && mFullRequestsFrom.size() >= MAX_RATE_ENTRIES)
            return;     // pruned every tick; only a flood of distinct requesters fills it
        mFullRequestsFrom[sender].reset();
        if (mLastFullOnRequest.getElapsedTimeF32() > REQUEST_COOLDOWN)
        {
            mLastFullOnRequest.reset();
            for (auto& entry : mOutgoing)
                entry.second.mFullPending = true;
        }
        return;
    }

    if (line.compare(0, 3, "R1|") == 0 || line.compare(0, 3, "E1|") == 0)
    {
        LLUUID target;
        if (!LLUUID::parseUUID(line.substr(3), &target) || target.isNull() || target == sender)
            return;
        if (line[0] == 'R')
        {
            // sender asked target. Bounded: per poser and overall (oldest first).
            if (LLMuteList::getInstance()->isMuted(sender))
                return;
            size_t from_sender = 0;
            for (const auto& entry : mPendingRequests)
                from_sender += (entry.first.first == sender) ? 1 : 0;
            if (from_sender >= MAX_PENDING_PER_POSER && !mPendingRequests.count(pair_t(sender, target)))
                return;
            while (mPendingRequests.size() >= MAX_PENDING)
            {
                auto oldest = mPendingRequests.begin();
                for (auto it = mPendingRequests.begin(); it != mPendingRequests.end(); ++it)
                {
                    if (it->second.getElapsedTimeF32() > oldest->second.getElapsedTimeF32())
                        oldest = it;
                }
                mPendingRequests.erase(oldest);
            }
            mPendingRequests[pair_t(sender, target)].reset();
        }
        else
        {
            revokeGrant(target, sender);                         // sender stopped posing target
        }
        return;
    }

    if (line.compare(0, 3, "P1|") == 0)
        handlePoseLine(sender, split(line, '|', 6));
}

void SLPoseSync::handlePoseLine(const LLUUID& sender, const std::vector<std::string>& fields)
{
    if (fields.size() != 6)
        return;
    if (fields[2] == "L")
    {
        // Look-at (#902): its own validation, gates and state (slposelookat); it never
        // makes or refreshes a pose remote and never asks for a full.
        if (SLPoseLookAt::instanceExists())
            SLPoseLookAt::instance().onRemoteLine(sender, fields[3], fields[4], fields[5]);
        return;
    }
    if (LLMuteList::getInstance()->isMuted(sender) || isIgnored(sender))
        return;

    LLUUID target = sender;
    if (fields[4] != "-")
    {
        // Posing someone else: only with a grant this viewer verified from the target
        // itself. The sender's word for it counts for nothing.
        if (!LLUUID::parseUUID(fields[4], &target) || !hasVerifiedGrant(target, sender))
            return;
    }
    // Others' poses need "show others"; a pose of us that we agreed to is always shown.
    if (target != gAgentID && !isShowingOthers())
        return;
    if (target != gAgentID && isIgnored(target))
        return;

    const std::string key = remoteKey(sender, target);
    const std::string& kind = fields[2];
    if (kind == "X")
    {
        dropRemote(key, true);
        return;
    }
    // Unknown kinds (a later protocol) and oversized data are ignored before any state is
    // made for the sender.
    if ((kind != "F" && kind != "D") || fields[5].size() > MAX_DATA_CHARS)
        return;

    U32 seq = 0;
    S32 part = 0, total = 0;
    const std::vector<std::string> part_fields = split(fields[3], '/', 2);
    if (!LLStringUtil::convertToU32(fields[1], seq) || part_fields.size() != 2 || !LLStringUtil::convertToS32(part_fields[0], part) ||
        !LLStringUtil::convertToS32(part_fields[1], total) || total < 1 || total > MAX_PARTS || part < 1 || part > total)
    {
        return;
    }

    if (!mRemotes.count(key) && mRemotes.size() >= MAX_REMOTES)
        return;     // enough people shown; newcomers wait until someone goes
    Remote& remote = mRemotes[key];
    const bool is_new = remote.mSender.isNull();
    remote.mSender = sender;
    remote.mTarget = target;
    remote.mLastHeard.reset();

    if (kind == "F")
    {
        if (remote.mFullSeq != (U16)seq || remote.mPartsExpected != total)
        {
            remote.mFullSeq = (U16)seq;
            remote.mPartsExpected = total;
            remote.mParts.clear();
        }
        std::string decoded;
        if (!SLPoseQuant::decode64(fields[5], decoded))
            return;
        remote.mParts[part] = decoded;
        if ((S32)remote.mParts.size() == total)
        {
            std::string data;
            for (const auto& [index, chunk] : remote.mParts)
                data += chunk;
            remote.mParts.clear();
            applyRecords(remote, data, true);
            remote.mHasBaseline = true;
            if (is_new)
                mChangedSignal();
        }
    }
    else if (kind == "D")
    {
        if (!remote.mHasBaseline)
        {
            requestFull(remote);
            return;
        }
        std::string decoded;
        if (SLPoseQuant::decode64(fields[5], decoded))
            applyRecords(remote, decoded, false);
    }
}

void SLPoseSync::applyRecords(Remote& remote, const std::string& data, bool full)
{
    LLVOAvatar* avatar = findAvatar(remote.mTarget);
    if (!avatar)
        return;
    // Our own avatar only takes a pose from someone we granted (checked by the caller),
    // and never while we pose ourselves in the studio.
    if (avatar->isSelf() && (remote.mSender == gAgentID || SLPoseStudio::instance().isPosedByStudio(gAgentID)))
        return;

    FSPoserAnimator& animator = SLPoseStudio::instance().getAnimator();
    // A fresh posing motion holds a joint state for every bone and collision volume. Bones
    // the sender did not pose must not keep one, or they override the avatar's own
    // animations (volume-bone animations in particular vanish for the viewer).
    const bool fresh_motion = !animator.isPosingAvatar(avatar);
    if (fresh_motion && !animator.tryPosingAvatar(avatar))
        return;

    const auto& joints = animator.PoserJoints;
    std::map<U8, BoneState> bones;
    S32 ease_ms = -1;
    const U8* p = (const U8*)data.data();
    const U8* end = p + data.size();
    while (end - p >= 2)
    {
        const U8 index = p[0];
        const U8 flags = p[1];
        p += 2;
        if (index == META_INDEX && flags == FLAG_POSITION)
        {
            if (end - p < META_PAYLOAD)
                return;
            if (p[0] == META_EASE)
                ease_ms = ((S32)p[1] << 8) | (S32)p[2];
            p += META_PAYLOAD;     // unknown meta types are skipped
            continue;
        }
        BoneState bone;
        bone.mPosed = !(flags & FLAG_RELEASED);
        if (flags & FLAG_ROTATION)
        {
            if (end - p < 6)
                return;
            bone.mRotation = SLPoseQuant::unpackQuat(p);
            p += 6;
        }
        if (flags & FLAG_POSITION)
        {
            if (end - p < 6)
                return;
            bone.mPosition.set(getS16(p), getS16(p + 2), getS16(p + 4));
            p += 6;
        }
        if (flags & FLAG_SCALE)
        {
            if (end - p < 6)
                return;
            bone.mScale.set(getS16(p), getS16(p + 2), getS16(p + 4));
            p += 6;
        }
        if (index < joints.size() && isSyncedJoint(joints[index]))
            bones[index] = bone;
    }

    // Ease into the pose like the sender's studio does: over the sent ease of a pose load /
    // sequence step, otherwise over a short smoothing so 2/s edits are not steppy. Blends
    // start from what is on screen now, so a line arriving mid-blend carries on from there.
    static LLCachedControl<U32> smooth_ms(gSavedSettings, "SLPoseSyncReceiveSmoothMs", 150);
    // A sent ease is the time the sender's blend had left, so it can be tiny: never ease
    // faster than the plain smoothing (an ease of exactly 0 still means "at once").
    U32 blend_ms = (ease_ms >= 0) ? (U32)ease_ms : (U32)smooth_ms;
    if (blend_ms > 0)
        blend_ms = llmax(blend_ms, (U32)smooth_ms);
    const F32 blend_time = llclamp((F32)blend_ms / 1000.f, 0.f, SLPoseStudio::MAX_EASE_IN);
    SLPoseStudio& studio = SLPoseStudio::instance();
    SLPoseStudio::blend_start_t from;
    if (blend_time > 0.01f)
        from = studio.captureBlendStart(avatar);
    std::set<std::string> posed;

    for (size_t i = 0; i < joints.size() && i < 256; ++i)
    {
        if (!isSyncedJoint(joints[i]))
        {
            // Collision volumes are never part of a shared pose: leave them to the animations.
            animator.setPosingAvatarJoint(avatar, joints[i], false);
            continue;
        }
        auto it = bones.find((U8)i);
        if (it == bones.end())
        {
            // A full pose names every posed bone; the rest play their own animation. A
            // motion that was just created holds every bone until told otherwise.
            if (full || fresh_motion)
            {
                studio.cancelBoneBlend(avatar->getID(), joints[i]);
                animator.setPosingAvatarJoint(avatar, joints[i], false);
            }
            continue;
        }
        const BoneState& bone = it->second;
        if (!bone.mPosed)
        {
            // Released bones go back to the animations at once (as in the studio).
            studio.cancelBoneBlend(avatar->getID(), joints[i]);
            animator.setPosingAvatarJoint(avatar, joints[i], false);
            continue;
        }
        animator.setJointTargetRotation(avatar, joints[i], bone.mRotation);
        animator.loadJointPosition(avatar, &joints[i], true, bone.mPosition);
        animator.loadJointScale(avatar, &joints[i], true, bone.mScale);
        posed.insert(joints[i].jointName());
    }

    // The bones now hold the received pose; ease them there from where they were.
    studio.startBlend(avatar, from, posed, blend_time, false);
}

void SLPoseSync::requestFull(Remote& remote)
{
    if (remote.mRequested && remote.mLastRequest.getElapsedTimeF32() < REMOTE_REQUEST_COOLDOWN)
        return;
    remote.mRequested = true;
    remote.mLastRequest.reset();
    SLBridge::instance().sendCommand("say|Q1|" + remote.mSender.asString());
}

void SLPoseSync::dropRemote(const std::string& key, bool stop_posing)
{
    auto it = mRemotes.find(key);
    if (it == mRemotes.end())
        return;
    const LLUUID target = it->second.mTarget;
    mRemotes.erase(it);

    // Another sender still poses this avatar (e.g. its own shared pose and a granted
    // poser): leave it posed; that sender's next full pose sets every bone again.
    for (const auto& entry : mRemotes)
    {
        if (entry.second.mTarget == target)
            stop_posing = false;
    }

    if (stop_posing && !SLPoseStudio::instance().isPosedByStudio(target))
    {
        SLPoseStudio::instance().cancelBlend(target);
        // Any region: an avatar that crossed away (or that we left behind) must not stay
        // frozen in a pose nobody updates.
        if (LLVOAvatar* avatar = findAnyAvatar(target))
            SLPoseStudio::instance().getAnimator().forceStopPosingAvatar(avatar);
    }
    mChangedSignal();
}

// ---------------------------------------------------------------------------
// Tick
// ---------------------------------------------------------------------------

void SLPoseSync::tick()
{
    if (!isAgentAvatarValid() || !gAgent.getRegion())
        return;

    updateBridgeUse();

    SLBridge& bridge = SLBridge::instance();
    SLPoseStudio& studio = SLPoseStudio::instance();
    LLVOAvatar* self = gAgentAvatarp.get();

    // Grants hold only while both people are here with us.
    std::vector<pair_t> gone;
    for (const pair_t& grant : mGrants)
    {
        if (!findAvatar(grant.first) || !findAvatar(grant.second))
            gone.push_back(grant);
    }
    for (const pair_t& grant : gone)
        revokeGrant(grant.second, grant.first);

    pruneConsent();

    // RLV: while sending on the public pose / consent-confirm channels is restricted, we
    // send no pose data and no grant renewals (revocations and stops always go out).
    const bool rlv_pose_ok = !RlvActions::isRlvEnabled() || RlvActions::canSendChannel(SLBridge::POSE_CHANNEL);
    if (bridge.isReady() && rlv_pose_ok)
    {
        // Our own avatar while sharing, and everyone who lets us pose them.
        // Only poses made in our own studio go out -- never a pose the sync layer applied
        // to us from someone else (that would echo it back as ours).
        tickOutgoing(self, isSharing() && studio.isPosedByStudio(gAgentID) && studio.isPosing(self), gAgentID);

        std::vector<LLUUID> targets;
        for (const auto& [id, out] : mOutgoing)
        {
            if (id != gAgentID)
                targets.push_back(id);
        }
        for (const pair_t& grant : mGrants)
        {
            if (grant.first == gAgentID && !mOutgoing.count(grant.second))
                targets.push_back(grant.second);
        }
        for (const LLUUID& id : targets)
        {
            LLVOAvatar* avatar = findAvatar(id);
            const bool active = avatar && hasVerifiedGrant(id, gAgentID) && studio.isPosedByStudio(id) && studio.isPosing(avatar);
            tickOutgoing(avatar, active, id);
            if (!active && !hasVerifiedGrant(id, gAgentID))
                mOutgoing.erase(id);
        }

        // As a Slipstream target, re-state our grants region-wide so people who arrive
        // later (or missed our dialog answer) can verify them.
        const std::vector<LLUUID> posers = getPosers();
        if (!posers.empty() && mLastRenewal.getElapsedTimeF32() > RENEW_INTERVAL)
        {
            mLastRenewal.reset();
            for (const LLUUID& poser : posers)
                sayGrant(poser);
        }
    }

    // Expire senders that went quiet (logged off, left, took the helper off) and
    // avatars that are no longer here.
    std::vector<std::string> expired;
    for (const auto& [key, remote] : mRemotes)
    {
        if (remote.mLastHeard.getElapsedTimeF32() > REMOTE_EXPIRY || !findAvatar(remote.mTarget))
            expired.push_back(key);
    }
    for (const std::string& key : expired)
        dropRemote(key, true);
}

// ---------------------------------------------------------------------------
// Consent
// ---------------------------------------------------------------------------

// static
std::string SLPoseSync::tagFor(const LLUUID& poser)
{
    // Same as the script: the first 8 characters of the key as LSL prints it (lowercase).
    return poser.asString().substr(0, 8);
}

bool SLPoseSync::hasVerifiedGrant(const LLUUID& target, const LLUUID& poser) const
{
    return target.notNull() && poser.notNull() && target != poser && mGrants.count(pair_t(poser, target)) != 0;
}

std::vector<LLUUID> SLPoseSync::getPosers() const
{
    std::vector<LLUUID> posers;
    for (const pair_t& grant : mGrants)
    {
        if (grant.second == gAgentID)
            posers.push_back(grant.first);
    }
    return posers;
}

void SLPoseSync::addGrant(const LLUUID& target, const LLUUID& poser)
{
    if (target.isNull() || poser.isNull() || target == poser)
        return;
    if (mGrants.size() >= MAX_GRANTS && !mGrants.count(pair_t(poser, target)))
        return;
    if (!mGrants.insert(pair_t(poser, target)).second)
        return;

    LLSD args;
    if (target == gAgentID)
    {
        // Someone may pose us now: renew it region-wide right away.
        mLastRenewal.reset();
        sayGrant(poser);
        if (notifyAllowed(poser))
        {
            args["NAME"] = LLSLURL("agent", poser, "inspect").getSLURLString();
            LLNotificationsUtil::add("SLPoseStudioBeingPosed", args);
        }
    }
    else if (poser == gAgentID)
    {
        mOutgoing[target].mFullPending = true;
        if (notifyAllowed(target))
        {
            args["NAME"] = LLSLURL("agent", target, "inspect").getSLURLString();
            LLNotificationsUtil::add("SLPoseStudioConsentGranted", args);
        }
        SLPoseStudio::instance().notifyChanged();
    }
    mChangedSignal();
}

void SLPoseSync::revokeGrant(const LLUUID& target, const LLUUID& poser)
{
    mGrantRenewals.erase(pair_t(poser, target));
    if (mGrants.erase(pair_t(poser, target)) == 0)
        return;

    // Whatever we were showing for that pair goes.
    dropRemotesFor(poser, target);

    LLSD args;
    if (poser == gAgentID)
    {
        if (notifyAllowed(target))
        {
            args["NAME"] = LLSLURL("agent", target, "inspect").getSLURLString();
            LLNotificationsUtil::add("SLPoseStudioConsentEnded", args);
        }
        // Stop posing them here too; the posing signal then tells everyone (endPosing).
        LLVOAvatar* avatar = findAvatar(target);
        SLPoseStudio& studio = SLPoseStudio::instance();
        if (avatar && studio.isPosing(avatar))
            studio.stopPosing(avatar);
        else
            endPosing(target);
        studio.notifyChanged();
    }
    else if (target == gAgentID && notifyAllowed(poser))
    {
        args["NAME"] = LLSLURL("agent", poser, "inspect").getSLURLString();
        LLNotificationsUtil::add("SLPoseStudioNoLongerPosed", args);
    }
    mChangedSignal();
}

void SLPoseSync::endPosing(const LLUUID& target)
{
    auto it = mOutgoing.find(target);
    const bool had_outgoing = (it != mOutgoing.end());
    const bool had_grant = hasVerifiedGrant(target, gAgentID);
    if (!had_outgoing && !had_grant)
        return;

    // Control lines: held until the bridge is ready, never dropped.
    if (had_outgoing && it->second.mAnnounced)
        sendStop(target.asString());
    SLBridge::instance().sendControl("end|" + target.asString());
    if (had_outgoing)
        mOutgoing.erase(it);
    if (had_grant)
        revokeGrant(target, gAgentID);
}

void SLPoseSync::stopBeingPosed()
{
    for (const LLUUID& poser : getPosers())
    {
        // Said by our avatar (and by our bridge for older viewers): no one shows it any more.
        sayRevoke(poser);
        revokeGrant(gAgentID, poser);
    }
}

void SLPoseSync::clearConsent()
{
    const std::set<pair_t> grants = mGrants;
    for (const pair_t& grant : grants)
        revokeGrant(grant.second, grant.first);
    mPendingRequests.clear();
}

void SLPoseSync::onRegionChanged()
{
    // Grants are tied to the region we verified them in: a teleport or crossing ends them.
    clearConsent();
    mAsked.clear();
}

bool SLPoseSync::requestPose(const LLUUID& target, std::string& reason)
{
    if (target.isNull() || target == gAgentID)
    {
        reason = "not_someone_else";
        return false;
    }
    if (!findAvatar(target))
    {
        reason = "not_here";
        return false;
    }
    if (hasVerifiedGrant(target, gAgentID))
    {
        reason = "already_granted";
        return false;
    }
    if (RlvActions::isRlvEnabled() && !RlvActions::canSendChannel(SLBridge::POSE_CHANNEL))
    {
        reason = "rlv";
        return false;
    }
    auto asked = mAsked.find(target);
    if (asked != mAsked.end() && asked->second.getElapsedTimeF32() < ASK_COOLDOWN)
    {
        reason = "wait";
        return false;
    }

    SLBridge& bridge = SLBridge::instance();
    mAsked[target].reset();     // also keeps the bridge on (see updateBridgeUse)
    updateBridgeUse();
    if (!bridge.isReady())
    {
        mAsked.erase(target);
        reason = (bridge.getState() == SLBridge::STATE_UNAVAILABLE) ? "unavailable" : "starting";
        // Keep the helper starting for a while so the next try works.
        mWarmup.reset();
        mWarmingUp = true;
        return false;
    }

    // Our own bridge does not hear its own R1, so record the pending request here too.
    mPendingRequests[pair_t(gAgentID, target)].reset();
    bridge.sendControl("ask|" + target.asString());
    return true;
}

bool SLPoseSync::isRequestBlocked(const LLUUID& poser) const
{
    return mBlockedRequesters.count(poser) != 0;
}

void SLPoseSync::setRequestBlocked(const LLUUID& poser, bool blocked)
{
    if (blocked && mBlockedRequesters.size() >= MAX_LIST_ENTRIES && !mBlockedRequesters.count(poser))
    {
        LL_WARNS("PoseSync") << "Blocked-requests list is full (" << MAX_LIST_ENTRIES << ")" << LL_ENDL;
        return;
    }
    if (blocked)
        mBlockedRequesters.insert(poser);
    else
        mBlockedRequesters.erase(poser);

    std::string list;
    for (const LLUUID& id : mBlockedRequesters)
        list += (list.empty() ? "" : ",") + id.asString();
    gSavedSettings.setString("SLPoseSyncBlockedRequesters", list);
    mChangedSignal();
}

bool SLPoseSync::onConsentDialog(const LLUUID& owner_id, const std::string& first_button)
{
    // Muted owners never get here (process_script_dialog drops them first). Only the two
    // dialogs a Slipstream bridge shows exist on this channel, each with its owner's own
    // tag; anything else (another object borrowing the channel) is a fake. Blocked people
    // get nothing shown at all.
    if (owner_id.isNull() || isRequestBlocked(owner_id))
        return false;
    const std::string tag = tagFor(owner_id);
    if (first_button == "Stop " + tag)
    {
        // How a target ends it from any viewer: always shown while that person may pose
        // us; otherwise it is noise and gets the request cooldown.
        if (hasVerifiedGrant(gAgentID, owner_id))
            return true;
    }
    else if (first_button != "Accept " + tag)
    {
        return false;
    }
    auto it = mDialogsFrom.find(owner_id);
    if (it != mDialogsFrom.end() && it->second.getElapsedTimeF32() < DIALOG_COOLDOWN)
        return false;
    if (it == mDialogsFrom.end() && mDialogsFrom.size() >= MAX_RATE_ENTRIES)
        return false;
    mDialogsFrom[owner_id].reset();
    if (first_button != "Accept " + tag)
        return true;

    // The sim filled in OwnerID: this is a real request from owner_id to us, whether or
    // not our bridge heard its R1 (it usually is not listening yet).
    mPendingRequests[pair_t(owner_id, gAgentID)].reset();

    // Our bridge must be listening for the poser's pose data once we accept.
    updateBridgeUse();

    LLSD args;
    args["NAME"] = LLSLURL("agent", owner_id, "inspect").getSLURLString();
    LLSD payload;
    payload["poser"] = owner_id;
    LLNotificationsUtil::add("SLPoseStudioRequestFrom", args, payload, [](const LLSD& notification, const LLSD& response)
        {
            if (LLNotificationsUtil::getSelectedOption(notification, response) == 1 && SLPoseSync::instanceExists())
                SLPoseSync::instance().setRequestBlocked(notification["payload"]["poser"].asUUID(), true);
        });
    return true;
}

void SLPoseSync::onBridgeConsent(const std::string& rest)
{
    // rest = <who>|<via>|<speaking object or avatar>|<message>, all sim-verified by the helper:
    //   via d  a dialog answer said by the avatar <who> on the consent channel
    //   via a  an attachment owned by <who> on the consent channel (G1 / S1)
    //   via v  the avatar <who> itself on the confirm channel (G1 / S1, consent v2)
    const std::vector<std::string> outer = split(rest, '|', 4);
    if (outer.size() != 4 || outer[1].size() != 1)
        return;
    LLUUID who, speaker;
    if (!LLUUID::parseUUID(outer[0], &who) || who.isNull() || !LLUUID::parseUUID(outer[2], &speaker))
        return;
    const char via = outer[1][0];
    const std::string& msg = outer[3];

    if (who == gAgentID)
    {
        // Our own bridge never hears its own statements, so a G1 attributed to us came from
        // some other attachment of ours: a grant we never gave unless we hold it.
        if (via == 'a' && msg.size() == 39 && msg.compare(0, 3, "G1|") == 0)
            onForgedGrant(speaker, msg.substr(3));
        return;
    }
    if (LLMuteList::getInstance()->isMuted(who))
        return;

    if (msg.compare(0, 3, "G1|") == 0 || msg.compare(0, 3, "S1|") == 0)
    {
        LLUUID poser;
        if (msg.size() != 39 || !LLUUID::parseUUID(msg.substr(3), &poser) || poser.isNull() || (via != 'a' && via != 'v'))
            return;
        if (msg[0] == 'G')
        {
            // Consent v2: a grant counts only when said by the avatar itself. Any of its
            // attachments can say G1 on the consent channel (older Slipstream viewers did
            // it that way), so that form proves nothing and is ignored.
            if (via != 'v')
                return;
            // who says poser may pose who. For us as the poser, only if we actually asked
            // who (or already have the grant): no unsolicited grants.
            if (poser == gAgentID && !mAsked.count(who) && !hasVerifiedGrant(who, gAgentID))
                return;
            addGrant(who, poser);
            mGrantRenewals[pair_t(poser, who)].reset();
        }
        else
        {
            // Stopping is always safe, whichever of who's objects says it.
            revokeGrant(who, poser);
        }
        return;
    }
    if (via != 'd')
        return;

    const std::string::size_type space = msg.find(' ');
    if (space == std::string::npos)
        return;
    const std::string verb = msg.substr(0, space);
    const std::string tag = msg.substr(space + 1);
    if (tag.size() != 8)
        return;

    if (verb == "Accept" || verb == "Deny")
    {
        // Pair the answer with the request it answers: a recent R1 from a poser whose
        // tag matches, to this very speaker.
        for (auto it = mPendingRequests.begin(); it != mPendingRequests.end(); ++it)
        {
            const LLUUID poser = it->first.first;
            const LLUUID target = it->first.second;
            if (target != who || tagFor(poser) != tag)
                continue;
            mPendingRequests.erase(it);
            if (verb == "Accept")
            {
                addGrant(target, poser);
            }
            else if (poser == gAgentID && notifyAllowed(target))
            {
                LLSD args;
                args["NAME"] = LLSLURL("agent", target, "inspect").getSLURLString();
                LLNotificationsUtil::add("SLPoseStudioRequestDenied", args);
            }
            break;
        }
    }
    else if (verb == "Stop")
    {
        std::vector<LLUUID> posers;
        for (const pair_t& grant : mGrants)
        {
            if (grant.second == who && tagFor(grant.first) == tag)
                posers.push_back(grant.first);
        }
        for (const LLUUID& poser : posers)
            revokeGrant(who, poser);
    }
}

void SLPoseSync::sayGrant(const LLUUID& poser)
{
    // Consent v2: said by our avatar on the confirm channel (100 m), which only this viewer
    // can do; older Slipstream viewers still listen for the bridge's G1 on the consent channel.
    if (RlvActions::isRlvEnabled() && !RlvActions::canSendChannel(SLBridge::CONFIRM_CHANNEL))
        return;
    SLBridge& bridge = SLBridge::instance();
    bridge.sayConfirm("G1|" + poser.asString());
    bridge.sendControl("csay|G1|" + poser.asString());
}

void SLPoseSync::sayRevoke(const LLUUID& poser)
{
    // Never held back (RLV or otherwise): stopping must always work.
    SLBridge& bridge = SLBridge::instance();
    bridge.sayConfirm("S1|" + poser.asString());
    bridge.sendControl("csay|S1|" + poser.asString());
}

void SLPoseSync::onForgedGrant(const LLUUID& object_id, const std::string& poser_str)
{
    LLUUID poser;
    if (!LLUUID::parseUUID(poser_str, &poser) || poser.isNull() || poser == gAgentID)
        return;
    if (hasVerifiedGrant(gAgentID, poser))
    {
        // We did agree to that one; the line was still not ours.
        LL_INFOS("PoseSync") << "Attachment " << object_id << " repeated our grant to " << poser << LL_ENDL;
        return;
    }
    LL_WARNS("PoseSync") << "Attachment " << object_id << " announced a pose grant to " << poser
                         << " that we never gave; revoking it" << LL_ENDL;
    // Undo it for everyone who might have believed it (older viewers do) -- once per poser
    // per cooldown: a repeating attachment must not turn our revocations into a flood
    // that crowds our own control lines out of the bridge queue.
    auto revoked = mForgedRevoked.find(poser);
    if (revoked == mForgedRevoked.end() || revoked->second.getElapsedTimeF32() > FORGED_REVOKE_COOLDOWN)
    {
        if (revoked == mForgedRevoked.end() && mForgedRevoked.size() >= MAX_FORGED_TRACKED)
            mForgedRevoked.erase(mForgedRevoked.begin());
        mForgedRevoked[poser].reset();
        sayRevoke(poser);
    }
    if (mForgedNotified && mForgedNotify.getElapsedTimeF32() < FORGED_NOTIFY_COOLDOWN)
        return;
    mForgedNotified = true;
    mForgedNotify.reset();

    std::string object_name = object_id.asString();
    if (LLViewerObject* object = gObjectList.findObject(object_id))
    {
        if (LLViewerObject* root = object->getRootEdit())
            object = root;
        if (const LLViewerInventoryItem* item = gInventory.getItem(object->getAttachmentItemID()))
            object_name = "\"" + item->getName() + "\"";
    }
    LLSD args;
    args["OBJECT"] = object_name;
    args["NAME"] = LLSLURL("agent", poser, "inspect").getSLURLString();
    LLNotificationsUtil::add("SLPoseStudioForgedGrant", args);
}

void SLPoseSync::dropRemotesFor(const LLUUID& sender, const LLUUID& target)
{
    dropRemote(remoteKey(sender, target), true);
}

bool SLPoseSync::notifyAllowed(const LLUUID& who)
{
    // At most one consent toast about a person every NOTIFY_COOLDOWN.
    auto it = mNotified.find(who);
    if (it != mNotified.end() && it->second.getElapsedTimeF32() < NOTIFY_COOLDOWN)
        return false;
    mNotified[who].reset();
    return true;
}

void SLPoseSync::onOwnConsentAnswer(const LLUUID& owner_id, const std::string& button)
{
    // Our own answer, applied locally: our bridge may not have been listening when we
    // clicked, and the sim's OwnerID is the poser's identity.
    if (owner_id.isNull() || owner_id == gAgentID)
        return;
    const std::string tag = tagFor(owner_id);
    if (button == "Accept " + tag)
    {
        if (!mPendingRequests.count(pair_t(owner_id, gAgentID)) || isRequestBlocked(owner_id))
            return;
        mPendingRequests.erase(pair_t(owner_id, gAgentID));
        addGrant(gAgentID, owner_id);
    }
    else if (button == "Deny " + tag)
    {
        mPendingRequests.erase(pair_t(owner_id, gAgentID));
    }
    else if (button == "Stop " + tag)
    {
        if (hasVerifiedGrant(gAgentID, owner_id))
        {
            sayRevoke(owner_id);
            revokeGrant(gAgentID, owner_id);
        }
    }
}

void SLPoseSync::onBridgeError(const std::string& rest)
{
    // err|ask|<key>: the person we asked is not reachable (left, not an avatar).
    if (rest.compare(0, 4, "ask|") != 0)
        return;
    LLUUID target;
    if (!LLUUID::parseUUID(rest.substr(4), &target))
        return;
    mAsked.erase(target);
    mPendingRequests.erase(pair_t(gAgentID, target));
    LLSD args;
    args["NAME"] = LLSLURL("agent", target, "inspect").getSLURLString();
    LLNotificationsUtil::add("SLPoseStudioRequestUnreachable", args);
}

void SLPoseSync::onBridgeReadyInRegion()
{
    // New region (or the bridge came back here): everyone around is new to us and we to
    // them. Ask once for their poses and re-announce ours.
    for (auto& entry : mOutgoing)
    {
        entry.second.mAnnounced = false;
        entry.second.mFullPending = true;
    }
    if (isShowingOthers())
    {
        mBroadcastRequested = true;
        mLastBroadcastRequest.reset();
        SLBridge::instance().sendControl("say|Q1|*");
    }
}

void SLPoseSync::pruneConsent()
{
    for (auto it = mPendingRequests.begin(); it != mPendingRequests.end();)
        it = (it->second.getElapsedTimeF32() > PENDING_EXPIRY) ? mPendingRequests.erase(it) : std::next(it);
    for (auto it = mAsked.begin(); it != mAsked.end();)
        it = (it->second.getElapsedTimeF32() > llmax(ASK_COOLDOWN, CONSENT_ACTIVE_WINDOW)) ? mAsked.erase(it) : std::next(it);
    for (auto it = mDialogsFrom.begin(); it != mDialogsFrom.end();)
        it = (it->second.getElapsedTimeF32() > llmax(DIALOG_COOLDOWN, CONSENT_ACTIVE_WINDOW)) ? mDialogsFrom.erase(it) : std::next(it);
    for (auto it = mNotified.begin(); it != mNotified.end();)
        it = (it->second.getElapsedTimeF32() > NOTIFY_COOLDOWN) ? mNotified.erase(it) : std::next(it);
    for (auto it = mFullRequestsFrom.begin(); it != mFullRequestsFrom.end();)
        it = (it->second.getElapsedTimeF32() > FULL_INTERVAL) ? mFullRequestsFrom.erase(it) : std::next(it);
    for (auto it = mForgedRevoked.begin(); it != mForgedRevoked.end();)
        it = (it->second.getElapsedTimeF32() > FORGED_REVOKE_COOLDOWN) ? mForgedRevoked.erase(it) : std::next(it);
    if (mWarmingUp && mWarmup.getElapsedTimeF32() > CONSENT_ACTIVE_WINDOW)
        mWarmingUp = false;

    // A Slipstream target re-states its grants every RENEW_INTERVAL; one that went quiet
    // (crashed, took the helper off, or someone else's grant we only saw renewed) ends.
    std::vector<pair_t> stale;
    for (const auto& [grant, timer] : mGrantRenewals)
    {
        if (grant.second != gAgentID && timer.getElapsedTimeF32() > GRANT_RENEW_EXPIRY)
            stale.push_back(grant);
    }
    for (const pair_t& grant : stale)
        revokeGrant(grant.second, grant.first);

    if (mStatsTimer.getElapsedTimeF32() > STATS_INTERVAL)
    {
        mStatsTimer.reset();
        LL_DEBUGS("PoseSync") << "remotes " << mRemotes.size() << " outgoing " << mOutgoing.size() << " grants " << mGrants.size()
                              << " renewals " << mGrantRenewals.size() << " pending " << mPendingRequests.size() << " asked " << mAsked.size()
                              << " dialogs " << mDialogsFrom.size() << " notified " << mNotified.size() << " q1 " << mFullRequestsFrom.size()
                              << " ignored " << mIgnored.size() << " blocked " << mBlockedRequesters.size()
                              << " bridge queue " << SLBridge::instance().getQueuedCommands() << LL_ENDL;
    }
}
