/**
 * @file sldiscordpresence.cpp
 * @brief Discord Rich Presence (#205), spoken directly over Discord's local IPC
 *        socket / named pipe. No Discord SDK and no third-party library.
 *
 * Protocol (the one Discord's own RPC clients use): every message is a frame of
 * little-endian uint32 opcode, uint32 payload length, then that many bytes of
 * UTF-8 JSON. Opcodes: 0 HANDSHAKE {"v":1,"client_id":...}, 1 FRAME (commands
 * and dispatches), 2 CLOSE, 3 PING (answer with PONG echoing the payload),
 * 4 PONG. After the handshake Discord sends a FRAME {"cmd":"DISPATCH",
 * "evt":"READY"}; from then on SET_ACTIVITY commands set or (activity null)
 * clear the presence. Closing the connection also clears it.
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

#include "sldiscordpresence.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include <boost/json.hpp>

#include "llagent.h"
#include "llagentdata.h"
#include "llapp.h"
#include "llappviewer.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llframetimer.h"
#include "llmath.h"
#include "llstartup.h"
#include "lluri.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llviewernetwork.h"
#include "llviewerregion.h"
#include "rlvhandler.h"

#if LL_WINDOWS
#include "llwin32headers.h"
#else
#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace
{
    // ------------------------------------------------------------------------
    // Protocol constants
    // ------------------------------------------------------------------------
    enum EOpcode : U32
    {
        OP_HANDSHAKE = 0,
        OP_FRAME     = 1,
        OP_CLOSE     = 2,
        OP_PING      = 3,
        OP_PONG      = 4,
    };

    constexpr size_t FRAME_HEADER_SIZE = 8;
    constexpr U32    MAX_FRAME_PAYLOAD = 64 * 1024;   // anything larger is a protocol error
    constexpr size_t MAX_RECV_BUFFER   = 256 * 1024;  // bounded receive buffer
    constexpr size_t MAX_TEXT_BYTES    = 128;         // Discord limit for details / state / large_text

    constexpr F64 TICK_INTERVAL      = 1.0;    // seconds between ticks
    constexpr F64 READY_TIMEOUT      = 10.0;   // handshake -> READY
    constexpr F64 BACKOFF_MIN        = 5.0;    // first reconnect delay
    constexpr F64 BACKOFF_MAX        = 60.0;   // reconnect delay cap (Discord not running)
    constexpr F64 BACKOFF_REJECTED   = 300.0;  // Discord closed us (e.g. invalid application id)

    constexpr const char* JOIN_BUTTON_LABEL = "Teleport to me";   // <= 32 chars
    constexpr const char* MAPS_URL_BASE     = "https://maps.secondlife.com/secondlife/";

    void put_le32(std::string& out, U32 v)
    {
        out.push_back((char)(v & 0xff));
        out.push_back((char)((v >> 8) & 0xff));
        out.push_back((char)((v >> 16) & 0xff));
        out.push_back((char)((v >> 24) & 0xff));
    }

    U32 get_le32(const std::string& in, size_t at)
    {
        return  (U32)(U8)in[at]
             | ((U32)(U8)in[at + 1] << 8)
             | ((U32)(U8)in[at + 2] << 16)
             | ((U32)(U8)in[at + 3] << 24);
    }

    // ------------------------------------------------------------------------
    // Transport: one non-blocking connection to the local Discord client.
    // ------------------------------------------------------------------------
    class DiscordIpc
    {
    public:
        ~DiscordIpc() { close(); }

        // Tries every candidate endpoint; on success fills path_out.
        bool connect(std::string& path_out);
        void close();
        bool isOpen() const;

        // Writes one frame. False on error (the caller drops the connection).
        bool sendFrame(U32 op, const std::string& payload);

        // Reads whatever is available without blocking. False on EOF / error.
        bool pump();

        // Pops one complete frame. Returns false when no complete frame is
        // buffered; sets error on a malformed / oversized frame.
        bool popFrame(U32& op, std::string& payload, bool& error);

    private:
        bool writeAll(const char* data, size_t len);

        std::string mRecv;
#if LL_WINDOWS
        HANDLE mPipe = INVALID_HANDLE_VALUE;
#else
        int mFd = -1;
#endif
    };

#if LL_WINDOWS

    bool DiscordIpc::connect(std::string& path_out)
    {
        close();
        for (int n = 0; n < 10; ++n)
        {
            const std::wstring name = L"\\\\.\\pipe\\discord-ipc-" + std::to_wstring(n);
            HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE)
            {
                mPipe = pipe;
                path_out = "\\\\.\\pipe\\discord-ipc-" + std::to_string(n);
                return true;
            }
        }
        return false;
    }

    void DiscordIpc::close()
    {
        if (mPipe != INVALID_HANDLE_VALUE)
        {
            CloseHandle(mPipe);
            mPipe = INVALID_HANDLE_VALUE;
        }
        mRecv.clear();
    }

    bool DiscordIpc::isOpen() const
    {
        return mPipe != INVALID_HANDLE_VALUE;
    }

    bool DiscordIpc::writeAll(const char* data, size_t len)
    {
        // Frames are a few hundred bytes, far below the pipe buffer, so a
        // synchronous write completes immediately.
        while (len > 0)
        {
            DWORD written = 0;
            if (!WriteFile(mPipe, data, (DWORD)len, &written, nullptr) || written == 0)
            {
                return false;
            }
            data += written;
            len -= written;
        }
        return true;
    }

    bool DiscordIpc::pump()
    {
        if (!isOpen())
        {
            return false;
        }
        char buf[4096];
        while (true)
        {
            DWORD avail = 0;
            if (!PeekNamedPipe(mPipe, nullptr, 0, nullptr, &avail, nullptr))
            {
                return false;   // broken pipe: Discord went away
            }
            if (avail == 0)
            {
                return true;
            }
            DWORD got = 0;
            const DWORD want = avail < (DWORD)sizeof(buf) ? avail : (DWORD)sizeof(buf);
            if (!ReadFile(mPipe, buf, want, &got, nullptr) || got == 0)
            {
                return false;
            }
            mRecv.append(buf, got);
            if (mRecv.size() > MAX_RECV_BUFFER)
            {
                return false;
            }
        }
    }

#else // Unix domain socket (Linux; would also work on macOS)

    std::vector<std::string> candidate_dirs()
    {
        std::vector<std::string> dirs;
        const char* xdg = getenv("XDG_RUNTIME_DIR");
        for (const char* var : { "XDG_RUNTIME_DIR", "TMPDIR", "TMP", "TEMP" })
        {
            const char* v = getenv(var);
            if (v && *v)
            {
                dirs.emplace_back(v);
            }
        }
        dirs.emplace_back("/tmp");
        if (xdg && *xdg)
        {
            const std::string x(xdg);
            dirs.push_back(x + "/app/com.discordapp.Discord");                 // Flatpak Discord
            dirs.push_back(x + "/.flatpak/com.discordapp.Discord/xdg-run");    // Flatpak Discord (alt)
            dirs.push_back(x + "/snap.discord");                               // Snap Discord
        }
        return dirs;
    }

    bool DiscordIpc::connect(std::string& path_out)
    {
        close();
        for (const std::string& dir : candidate_dirs())
        {
            for (int n = 0; n < 10; ++n)
            {
                const std::string path = dir + "/discord-ipc-" + std::to_string(n);
                sockaddr_un addr;
                memset(&addr, 0, sizeof(addr));
                addr.sun_family = AF_UNIX;
                if (path.size() >= sizeof(addr.sun_path))
                {
                    continue;
                }
                memcpy(addr.sun_path, path.c_str(), path.size());

                const int fd = socket(AF_UNIX, SOCK_STREAM, 0);
                if (fd < 0)
                {
                    return false;
                }
                fcntl(fd, F_SETFD, FD_CLOEXEC);
                const int fl = fcntl(fd, F_GETFL, 0);
                fcntl(fd, F_SETFL, (fl < 0 ? 0 : fl) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
                int one = 1;
                setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
                // A local stream socket connects immediately or fails (ENOENT /
                // ECONNREFUSED for a stale socket file, EAGAIN for a full backlog).
                if (::connect(fd, (const sockaddr*)&addr, sizeof(addr)) == 0)
                {
                    mFd = fd;
                    path_out = path;
                    return true;
                }
                ::close(fd);
            }
        }
        return false;
    }

    void DiscordIpc::close()
    {
        if (mFd >= 0)
        {
            ::close(mFd);
            mFd = -1;
        }
        mRecv.clear();
    }

    bool DiscordIpc::isOpen() const
    {
        return mFd >= 0;
    }

    bool DiscordIpc::writeAll(const char* data, size_t len)
    {
#ifdef MSG_NOSIGNAL
        constexpr int SEND_FLAGS = MSG_NOSIGNAL;
#else
        constexpr int SEND_FLAGS = 0;
#endif
        int waits = 0;
        while (len > 0)
        {
            const ssize_t n = ::send(mFd, data, len, SEND_FLAGS);
            if (n > 0)
            {
                data += n;
                len -= (size_t)n;
                continue;
            }
            if (n < 0 && errno == EINTR)
            {
                continue;
            }
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) && waits < 5)
            {
                // Socket buffer full (Discord not reading): wait briefly, at
                // most 5 x 20 ms, then give up on this connection.
                ++waits;
                pollfd pfd;
                pfd.fd = mFd;
                pfd.events = POLLOUT;
                pfd.revents = 0;
                poll(&pfd, 1, 20);
                continue;
            }
            return false;
        }
        return true;
    }

    bool DiscordIpc::pump()
    {
        if (!isOpen())
        {
            return false;
        }
        char buf[4096];
        while (true)
        {
            const ssize_t n = ::recv(mFd, buf, sizeof(buf), 0);
            if (n > 0)
            {
                mRecv.append(buf, (size_t)n);
                if (mRecv.size() > MAX_RECV_BUFFER)
                {
                    return false;
                }
                continue;
            }
            if (n == 0)
            {
                return false;   // EOF: Discord closed the socket
            }
            if (errno == EINTR)
            {
                continue;
            }
            return errno == EAGAIN || errno == EWOULDBLOCK;
        }
    }

#endif // LL_WINDOWS

    bool DiscordIpc::sendFrame(U32 op, const std::string& payload)
    {
        if (!isOpen())
        {
            return false;
        }
        std::string frame;
        frame.reserve(FRAME_HEADER_SIZE + payload.size());
        put_le32(frame, op);
        put_le32(frame, (U32)payload.size());
        frame.append(payload);
        return writeAll(frame.data(), frame.size());
    }

    bool DiscordIpc::popFrame(U32& op, std::string& payload, bool& error)
    {
        error = false;
        if (mRecv.size() < FRAME_HEADER_SIZE)
        {
            return false;
        }
        const U32 len = get_le32(mRecv, 4);
        if (len > MAX_FRAME_PAYLOAD)
        {
            error = true;
            return false;
        }
        if (mRecv.size() < FRAME_HEADER_SIZE + len)
        {
            return false;
        }
        op = get_le32(mRecv, 0);
        payload.assign(mRecv, FRAME_HEADER_SIZE, len);
        mRecv.erase(0, FRAME_HEADER_SIZE + len);
        return true;
    }

    // ------------------------------------------------------------------------
    // JSON helpers
    // ------------------------------------------------------------------------
    std::string json_string_field(const boost::json::value& v, const char* key)
    {
        if (const boost::json::object* obj = v.if_object())
        {
            if (const boost::json::value* f = obj->if_contains(key))
            {
                if (f->is_string())
                {
                    return std::string(f->as_string().c_str());
                }
            }
        }
        return std::string();
    }

    std::string clamp_text(const std::string& s)
    {
        return s.size() > MAX_TEXT_BYTES ? utf8str_truncate(s, (S32)MAX_TEXT_BYTES) : s;
    }

    // ------------------------------------------------------------------------
    // Presence state machine (main thread only)
    // ------------------------------------------------------------------------
    enum class EState
    {
        Closed,       // not connected; reconnect at mNextConnectTime
        AwaitReady,   // handshake sent, waiting for the READY dispatch
        Ready,        // can send SET_ACTIVITY
    };

    class Presence
    {
    public:
        void tick(F64 now);
        // Best-effort clear of the activity, then close. Resets the backoff so
        // a re-enable connects on the next tick.
        void disconnect();

        bool mShutdown = false;

    private:
        void dropConnection(F64 now, F64 retry_delay, const char* why);
        void serviceIncoming(F64 now);
        bool buildActivity(boost::json::object& activity);
        bool sendActivity(const boost::json::value& activity);

        DiscordIpc  mIpc;
        EState      mState = EState::Closed;
        std::string mPath;
        F64         mNextConnectTime = 0.0;
        F64         mBackoff = 0.0;
        F64         mHandshakeTime = 0.0;
        F64         mLastSendTime = 0.0;
        bool        mSentOnConnection = false;
        std::string mLastSent;           // serialized activity last sent on this connection
        U32         mNonce = 0;
        std::time_t mLoginTime = 0;      // timestamps.start; set at the first logged-in tick
        bool        mLoggedNoAppId = false;
    };

    Presence sPresence;

    void Presence::dropConnection(F64 now, F64 retry_delay, const char* why)
    {
        const bool was_ready = (mState == EState::Ready);
        mIpc.close();
        mState = EState::Closed;
        mLastSent.clear();
        mSentOnConnection = false;
        mNextConnectTime = now + retry_delay;
        if (was_ready)
        {
            LL_INFOS("Discord") << "Discord presence: disconnected (" << why << ")" << LL_ENDL;
        }
    }

    void Presence::disconnect()
    {
        if (mState == EState::Ready && mSentOnConnection)
        {
            sendActivity(boost::json::value(nullptr));   // best-effort; closing clears it too
        }
        if (mState == EState::Ready)
        {
            LL_INFOS("Discord") << "Discord presence: disconnected" << LL_ENDL;
        }
        mIpc.close();
        mState = EState::Closed;
        mLastSent.clear();
        mSentOnConnection = false;
        mNextConnectTime = 0.0;
        mBackoff = 0.0;
    }

    bool Presence::sendActivity(const boost::json::value& activity)
    {
        boost::json::object args;
        args["pid"] = LLApp::getPid();
        args["activity"] = activity;

        boost::json::object cmd;
        cmd["cmd"] = "SET_ACTIVITY";
        cmd["args"] = std::move(args);
        cmd["nonce"] = std::to_string(LLApp::getPid()) + "-" + std::to_string(++mNonce);
        return mIpc.sendFrame(OP_FRAME, boost::json::serialize(cmd));
    }

    void Presence::serviceIncoming(F64 now)
    {
        if (!mIpc.pump())
        {
            dropConnection(now, mBackoff, "connection lost");
            return;
        }

        U32 op = 0;
        std::string payload;
        bool error = false;
        while (mIpc.isOpen() && mIpc.popFrame(op, payload, error))
        {
            switch (op)
            {
            case OP_PING:
                if (!mIpc.sendFrame(OP_PONG, payload))
                {
                    dropConnection(now, mBackoff, "write failed");
                    return;
                }
                break;

            case OP_CLOSE:
            {
                boost::system::error_code ec;
                const boost::json::value v = boost::json::parse(payload, ec);
                const std::string msg = ec ? std::string() : json_string_field(v, "message");
                // Typically an invalid application id; retry only rarely.
                LL_INFOS("Discord") << "Discord presence: Discord closed the connection"
                                    << (msg.empty() ? std::string() : ": " + msg) << LL_ENDL;
                dropConnection(now, BACKOFF_REJECTED, "closed by Discord");
                return;
            }

            case OP_FRAME:
            {
                boost::system::error_code ec;
                const boost::json::value v = boost::json::parse(payload, ec);
                if (ec)
                {
                    break;
                }
                const std::string evt = json_string_field(v, "evt");
                if (evt == "READY" && mState == EState::AwaitReady)
                {
                    mState = EState::Ready;
                    mBackoff = 0.0;
                    mLastSent.clear();
                    mSentOnConnection = false;
                    LL_INFOS("Discord") << "Discord presence: connected (" << mPath << ")" << LL_ENDL;
                }
                else if (evt == "ERROR")
                {
                    // e.g. a rejected SET_ACTIVITY field; keep the connection.
                    LL_DEBUGS("Discord") << "Discord presence: error reply: " << payload << LL_ENDL;
                }
                break;
            }

            default:   // PONG and anything unknown
                break;
            }
        }
        if (error)
        {
            dropConnection(now, mBackoff, "protocol error");
        }
    }

    bool Presence::buildActivity(boost::json::object& activity)
    {
        LLViewerRegion* region = gAgent.getRegion();
        if (!region)
        {
            return false;
        }

        static LLCachedControl<bool> show_region(gSavedSettings, "SlipstreamDiscordShowRegion", false);
        static LLCachedControl<bool> show_activity(gSavedSettings, "SlipstreamDiscordShowActivity", true);
        static LLCachedControl<bool> show_name(gSavedSettings, "SlipstreamDiscordShowName", false);
        static LLCachedControl<bool> show_join(gSavedSettings, "SlipstreamDiscordShowJoin", false);
        static LLCachedControl<bool> hide_mature(gSavedSettings, "SlipstreamDiscordHideMatureLocation", true);

        LLGridManager* grids = LLGridManager::getInstance();
        const bool main_grid = grids->isInProductionGrid();   // maps.secondlife.com only knows agni
        const std::string grid_label = grids->getGridLabel();
        const std::string grid_text = main_grid ? std::string("In Second Life") : "On " + grid_label;

        // What the avatar is doing. Busy / Away override the posture.
        std::string doing;
        if (show_activity)
        {
            if (gAgent.isDoNotDisturb())
                doing = "Busy";
            else if (gAgent.getAFK())
                doing = "Away";
            else if (gAgent.isSitting())
                doing = "Sitting";
            else if (gAgent.getFlying())
                doing = "Flying";
            else
                doing = "Standing";
        }

        // Avatar name, unless RLVa hides names. Omitted until the name cache
        // has our own name (the next tick picks it up).
        std::string name;
        if (show_name
            && !gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMES)
            && !gRlvHandler.hasBehaviour(RLV_BHVR_SHOWNAMETAGS))
        {
            LLAvatarName av_name;
            if (LLAvatarNameCache::get(gAgentID, &av_name))
            {
                name = av_name.getCompleteName();
            }
        }

        // Location: never under RLVa @showloc, and (by default) only in General
        // regions. Unknown access (just after login / teleport) counts as hidden.
        const bool location_allowed = !gRlvHandler.hasBehaviour(RLV_BHVR_SHOWLOC)
                                      && (!hide_mature || region->getSimAccess() == SIM_ACCESS_PG);
        const std::string region_name = region->getName();
        const bool region_shown = show_region && location_allowed && !region_name.empty();
        const bool join_shown = region_shown && show_join && main_grid;

        std::string details;
        if (!doing.empty() && !name.empty())
            details = doing + " as " + name;
        else if (!doing.empty())
            details = doing;
        else if (!name.empty())
            details = "Playing as " + name;
        else
            details = grid_text;

        std::string state;
        if (region_shown)
            state = "In " + region_name + (main_grid ? std::string() : " (" + grid_label + ")");
        else if (details != grid_text)
            state = grid_text;

        activity["details"] = clamp_text(details);
        if (!state.empty())
        {
            activity["state"] = clamp_text(state);
        }

        boost::json::object assets;
        assets["large_image"] = SLDiscordPresence::LARGE_IMAGE_KEY;
        assets["large_text"] = clamp_text(LLVersionInfo::instance().getChannelAndVersion());
        activity["assets"] = std::move(assets);

        boost::json::object timestamps;
        timestamps["start"] = (std::int64_t)mLoginTime;
        activity["timestamps"] = std::move(timestamps);

        if (join_shown)
        {
            // Quantized to 4 m so walking around doesn't change the activity
            // (and cost an update) every tick.
            const LLVector3 pos = gAgent.getPositionAgent();
            const S32 x = llclamp(ll_round(pos.mV[VX] / 4.f) * 4, 0, 255);
            const S32 y = llclamp(ll_round(pos.mV[VY] / 4.f) * 4, 0, 255);
            const S32 z = llclamp(ll_round(pos.mV[VZ] / 4.f) * 4, 0, 4096);
            const std::string url = MAPS_URL_BASE + LLURI::escape(region_name)
                                    + llformat("/%d/%d/%d", x, y, z);
            if (url.size() <= 512)
            {
                boost::json::object button;
                button["label"] = JOIN_BUTTON_LABEL;
                button["url"] = url;
                boost::json::array buttons;
                buttons.push_back(std::move(button));
                activity["buttons"] = std::move(buttons);
            }
        }
        return true;
    }

    void Presence::tick(F64 now)
    {
        static LLCachedControl<bool> enabled(gSavedSettings, "SlipstreamDiscordEnabled", false);
        static LLCachedControl<std::string> app_id_setting(gSavedSettings, "SlipstreamDiscordAppID", "");

        // Nothing before login completes, nothing after disconnect.
        const bool logged_in = (LLStartUp::getStartupState() == STATE_STARTED) && !gDisconnected;
        if (!enabled || !logged_in)
        {
            if (mState != EState::Closed || mIpc.isOpen())
            {
                disconnect();
            }
            return;
        }

        std::string app_id = app_id_setting;
        LLStringUtil::trim(app_id);
        if (app_id.empty())
        {
            if (!mLoggedNoAppId)
            {
                mLoggedNoAppId = true;
                LL_INFOS("Discord") << "Discord presence: no application id configured" << LL_ENDL;
            }
            if (mState != EState::Closed || mIpc.isOpen())
            {
                disconnect();
            }
            return;
        }

        if (mLoginTime == 0)
        {
            mLoginTime = std::time(nullptr);
        }

        if (mState == EState::Closed)
        {
            if (now < mNextConnectTime)
            {
                return;
            }
            mBackoff = (mBackoff <= 0.0) ? BACKOFF_MIN : llmin(mBackoff * 2.0, BACKOFF_MAX);
            if (!mIpc.connect(mPath))
            {
                mNextConnectTime = now + mBackoff;   // Discord not running: quietly retry
                return;
            }
            boost::json::object hs;
            hs["v"] = 1;
            hs["client_id"] = app_id;
            if (!mIpc.sendFrame(OP_HANDSHAKE, boost::json::serialize(hs)))
            {
                dropConnection(now, mBackoff, "handshake write failed");
                return;
            }
            mState = EState::AwaitReady;
            mHandshakeTime = now;
        }

        serviceIncoming(now);

        if (mState == EState::AwaitReady && now - mHandshakeTime > READY_TIMEOUT)
        {
            dropConnection(now, mBackoff, "no READY");
            return;
        }
        if (mState != EState::Ready)
        {
            return;
        }

        boost::json::object activity;
        if (!buildActivity(activity))
        {
            return;
        }
        const std::string serialized = boost::json::serialize(activity);
        if (serialized == mLastSent)
        {
            return;
        }
        if (mSentOnConnection && now - mLastSendTime < SLDiscordPresence::MIN_UPDATE_INTERVAL)
        {
            return;   // coalesced: the latest content goes out once the interval has passed
        }
        if (!sendActivity(boost::json::value(std::move(activity))))
        {
            dropConnection(now, mBackoff, "write failed");
            return;
        }
        mLastSent = serialized;
        mLastSendTime = now;
        mSentOnConnection = true;
    }
} // namespace

namespace SLDiscordPresence
{
    void idle()
    {
        if (sPresence.mShutdown)
        {
            return;
        }
        static F64 s_next_tick = 0.0;
        const F64 now = LLFrameTimer::getTotalSeconds();
        if (now < s_next_tick)
        {
            return;
        }
        s_next_tick = now + TICK_INTERVAL;
        sPresence.tick(now);
    }

    void shutdown()
    {
        if (sPresence.mShutdown)
        {
            return;
        }
        sPresence.disconnect();
        sPresence.mShutdown = true;
    }
}
