/**
 * @file slbridge.h
 * @brief The "#Slipstream Helper" bridge (#500, #428): an on-demand scripted
 *        attachment that routes messages between the viewer and the region.
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

#ifndef SL_BRIDGE_H
#define SL_BRIDGE_H

#include <deque>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <boost/signals2.hpp>

#include "llsingleton.h"
#include "lltimer.h"
#include "lluuid.h"
#include "llvoinventorylistener.h"

class LLEventTimer;
class LLViewerObject;

/**
 * Lifecycle (one state machine, ticked four times a second):
 *   IDLE        no feature wants the bridge; it is not worn (a helper still worn from an
 *               earlier session is taken off).
 *   FIND        wait until the "#Slipstream" folder (in the inventory root only) is fetched,
 *               then use the "#Slipstream Helper" this viewer made for the account (its item
 *               id is kept in the per-account settings); create the folder / copy a library
 *               prim only when it is truly missing -- at most twice each per session. A
 *               helper item the user cannot modify is never used.
 *   ATTACH      wear it on the HUD (Center 2); RLV attach locks are checked first.
 *   HANDSHAKE   ask for the script's "#slb:hello" (version, private channel, object key):
 *               "hello" on the discovery channel derived from our agent id, every
 *               HELLO_ASK_INTERVAL. The script never speaks first, so a helper left worn
 *               under another viewer stays silent. If we still know its private channel we
 *               probe that too before deciding a reinstall is needed. A current hello is
 *               then checked against the object itself (VERIFY) before anything is routed;
 *               a helper not yet recorded as ours always gets a fresh install first.
 *   INSTALL     no or outdated script: remove old scripts from the object, create the script
 *               in inventory, upload it (UpdateScriptAgent) and drop it into the object.
 *   VERIFY      the worn helper must be a single prim whose inventory holds exactly one
 *               script, ours (name and creator); otherwise it is reinstalled once, then
 *               refused (the user is told).
 *   READY       commands flow (pinged every PING_INTERVAL so the script keeps forwarding);
 *               handlers receive routed "#slb:<type>" lines.
 *   UNAVAILABLE scripts do not run here (no-script land), RLV forbids it, the script did not
 *               compile, the user keeps taking it off, ... -- retried on region change and
 *               (SLBridgeAutoRetry) on a 30 s .. 5 min backoff, except when the user took it
 *               off, it is in the Trash, or the script did not compile. <GMDX> A worn, paused
 *               helper is also asked for hello every 15 s (no reinstall when it answers).
 * When the last feature releases the bridge it is detached back to inventory; it is also
 * detached on a clean logout (<GMDX> one still being attached when the logout starts stays
 * worn; the next GunMetal-DX login takes it off). Any other worn "#Slipstream Helper" is taken off.
 *
 * Only lines from the worn helper object itself are acted on; any other object of ours
 * that says "#slb:" is swallowed and ignored.
 */
class SLBridge : public LLSingleton<SLBridge>, public LLVOInventoryListener
{
    LLSINGLETON(SLBridge);
    ~SLBridge() override;
    LOG_CLASS(SLBridge);

public:
    enum EState
    {
        STATE_IDLE = 0,
        STATE_FIND,
        STATE_ATTACH,
        STATE_HANDSHAKE,
        STATE_INSTALL,
        STATE_VERIFY,
        STATE_READY,
        STATE_UNAVAILABLE,
    };

    static constexpr S32 POSE_CHANNEL = -1397510227;     // -0x534C5053 "SLPS"
    static constexpr S32 CONSENT_CHANNEL = -1397510211;  // -0x534C5043 "SLPC"
    // Consent v2: a target's own viewer states grants (G1) and revocations (S1) as chat
    // from the avatar itself, which no attachment can produce. Avatars cannot chat on
    // negative channels, so this is the positive twin of CONSENT_CHANNEL.
    static constexpr S32 CONFIRM_CHANNEL = 1397510211;   // 0x534C5043 "SLPC"
    static constexpr size_t MAX_COMMAND_BYTES = 1000;    // chat is capped at 1023 bytes

    // After login: starts the ticker (which also takes off a helper left worn).
    void init();
    void detachOnLogout();  // <GMDX> before the logout request goes out

    // Features ask for the bridge by name; it is worn while anyone holds it.
    void addUser(const std::string& feature);
    void removeUser(const std::string& feature);
    bool isWanted() const { return !mUsers.empty(); }

    // While a feature needs the bridge, the worn helper can't be taken off by the user
    // (inventory Detach, Take Off, outfit replacement, Detach on the selected HUD): the
    // detach paths ask these. Our own detach (feature released, logout) is not blocked.
    bool isProtectedItem(const LLUUID& item_id) const;
    bool isProtectedObject(const LLViewerObject* object) const;
    // Any #Slipstream Helper (ours, a stray, one not yet identified at login): never drawn
    // locally -- it is a tiny invisible HUD, but a stored copy that never got its script
    // (or its hidden prim state) shows as the library rock in front of the camera.
    // Never pickable or selectable either (#537): it sits on a HUD point, and clicks and
    // edit selections pass through it to whatever is behind.
    bool isHelperObject(const LLViewerObject* object) const;

    // Tell the user why the helper stays on (rate-limited).
    void notifyProtected() const;
    // The user explicitly asked to take the helper itself off (inventory Take Off, which
    // goes through an outfit update): the outfit-update path notifies only then, never
    // for an outfit change that merely leaves the helper out.
    void noteExplicitRemoval(const LLUUID& item_id);
    bool takeExplicitRemoval();

    EState getState() const { return mState; }
    bool isReady() const { return mState == STATE_READY; }
    const std::string& getReason() const { return mReason; }
    const LLUUID& getObjectID() const { return mObjectID; }
    // Version of the script the viewer carries: a hash of its source, so any change to
    // slipstream_helper.lsl makes worn helpers update.
    S32 getScriptVersion() const;

    // Data commands ("say|..." pose lines): paced, dropped when not ready, oldest dropped
    // first when the queue is full (newer pose data supersedes it).
    bool sendCommand(const std::string& command);
    // Control commands (revocations, ends, asks): held until the bridge is ready, sent
    // before any data, never dropped by state changes.
    void sendControl(const std::string& command);
    size_t getQueuedCommands() const { return mQueue.size() + mControlQueue.size(); }

    // A consent statement said by our own avatar (shout range) on CONFIRM_CHANNEL. Not a
    // bridge command: it works whatever state the helper is in. Grants (G1) are not said
    // while RLV forbids sending on that channel; revocations (S1) always are.
    void sayConfirm(const std::string& line);

    // Routed "#slb:<type>|<rest>" lines from the bridge. One handler per type.
    typedef std::function<void(const std::string& rest)> handler_t;
    void registerHandler(const std::string& type, handler_t handler);

    typedef boost::signals2::signal<void(EState)> state_signal_t;
    boost::signals2::connection setStateCallback(const state_signal_t::slot_type& cb) { return mStateSignal.connect(cb); }
    // Fired when the bridge became ready again in a new region (teleport / crossing).
    typedef boost::signals2::signal<void()> region_signal_t;
    boost::signals2::connection setReadyInRegionCallback(const region_signal_t::slot_type& cb) { return mReadyInRegionSignal.connect(cb); }

    // From llviewermessage: llOwnerSay of an object we own. Returns true if consumed.
    bool processOwnerSay(const LLUUID& object_id, const std::string& message);

    // Called on region change (teleport / crossing) to retry an unavailable bridge.
    void onRegionChanged();

    // LLVOInventoryListener
    void inventoryChanged(LLViewerObject* object, LLInventoryObject::object_list_t* inventory, S32 serial_num, void* user_data) override;

private:
    void setState(EState state, const std::string& reason = std::string());
    void tick();
    void ensureTicker();
    void tickFind();
    static bool autoRetry();    // SLBridgeAutoRetry (#902)

    LLUUID findFolder() const;
    bool isHelperInTrash() const;
    LLUUID findHelperItem(const LLUUID& folder_id) const;
    bool isHelperItem(const LLUUID& item_id) const;
    // The helper item this viewer made / verified for the account (per-account setting).
    LLUUID getTrustedItem() const;
    void setTrustedItem(const LLUUID& item_id);
    // An item we may use as the helper: an object we own and may modify.
    bool isUsableHelperItem(const LLUUID& item_id) const;
    void rejectHelper(const LLUUID& item_id, const std::string& why);
    void beginVerify(LLViewerObject* object);
    void onVerifyInventory(LLViewerObject* object, LLInventoryObject::object_list_t* inventory);
    void createFolder();
    void createHelperItem(const LLUUID& folder_id);
    void attachHelper(const LLUUID& item_id);
    void detachHelper();
    void detachStrayHelpers();
    void refuseTrashed();
    LLViewerObject* getWornHelper() const;
    void beginInstall(LLViewerObject* object);
    void createScriptItem();
    void uploadScript(const LLUUID& item_id, U32 install_gen);                       // <GMDX> gen
    void onScriptUploaded(const LLUUID& item_id, const LLSD& response, U32 install_gen);
    // <GMDX> Which install a create/upload callback belongs to: one from an install that was
    // overtaken (timeout, helper gone, a newer install) must not act on the current state.
    U32  mInstallGen = 0;
    bool mConfirmHello = false;         // <GMDX> READY straight after a drop: ask once more soon
    std::vector<LLUUID> mOldScripts;    // <GMDX> removed only when the new script is ready to drop
    S32  mRetryInstalls = 0;            // <GMDX> reinstalls started from UNAVAILABLE in this region
    bool isCurrentInstall(U32 gen) const { return gen == mInstallGen && mState == STATE_INSTALL; }
    std::string loadScriptSource() const;
    void sendRawChat(const std::string& text, S32 channel);

    std::set<std::string> mUsers;
    EState mState = STATE_IDLE;
    std::string mReason;
    LLTimer mStateTimer;        // time in the current state
    LLTimer mStaleCheckTimer;
    LLTimer mPingTimer;
    bool mBusy = false;         // an async inventory/attach step is in flight
    S32 mAttachAttempts = 0;
    S32 mRetries = 0;           // automatic retries out of UNAVAILABLE since the last READY
    S32 mFolderCreates = 0;     // per session
    S32 mHelperCopies = 0;      // per session
    S32 mUserDetaches = 0;      // the user took the helper off while it was needed
    bool mUserRefused = false;  // ... often enough that we stopped putting it back
    bool mSelfDetach = false;   // our own detachHelper() is running: not protected
    mutable LLTimer mProtectNoticeTimer;
    mutable bool mProtectNoticeShown = false;
    LLTimer mExplicitRemovalTimer;
    bool mExplicitRemoval = false;  // noteExplicitRemoval, taken by the next outfit update
    LLUUID mHiddenObjectID;     // helper object whose local instances were released
    bool mFolderFetchRequested = false;
    bool mLibraryFetchRequested = false;
    bool mProbeSent = false;
    S32  mHelloAsks = 0;        // discovery hellos sent in this handshake (or since the install drop)
    bool mNewRegion = false;    // a region change since the bridge was last ready

    LLUUID mFolderID;
    LLUUID mItemID;
    LLUUID mObjectID;
    S32 mChannel = 0;           // kept across re-handshakes so a lost hello can be probed
    S32 mReportedVersion = 0;
    bool mInventoryRequested = false;
    bool mVerifyRequested = false;  // the inventory answer is for VERIFY, not an install
    S32 mVerifyReinstalls = 0;      // installs forced by a failed check, this attach
    bool mForceInstall = false;     // a helper not recorded as ours: install before use
    bool mScriptSaved = false;      // INSTALL: the script is in the object, ask for hello
    LLTimer mConfirmTimer;          // sayConfirm budget window
    S32 mConfirmLines = 0;
    LLTimer mRouteClock;            // budget for routed helper lines
    F32 mRouteTokens = 80.f;
    F32 mRouteLast = 0.f;
    U32 mRouteDropped = 0;
    mutable LLUUID mTrustedItem;    // cache of SLBridgeHelperItem
    mutable LLTimer mTrustedReload;
    mutable bool mTrustedLoaded = false;
    std::set<LLUUID> mRejectedItems;    // look-alikes refused this session

    std::deque<std::string> mQueue;
    std::deque<std::string> mControlQueue;
    LLTimer mSendTimer;

    std::map<std::string, handler_t> mHandlers;
    state_signal_t mStateSignal;
    region_signal_t mReadyInRegionSignal;
    boost::signals2::connection mRegionConnection;
    LLEventTimer* mTicker = nullptr;
    mutable S32 mScriptVersion = 0;
};

#endif // SL_BRIDGE_H
