/**
 * @file sltestctl.cpp
 * @brief Developer test-control channel (compile-time opt-in, never in releases).
 *
 * See sltestctl.h. One network thread owns the socket; every command is posted to the
 * "mainloop" work queue and runs on the main thread. A command finishes by filling its
 * Pending slot; the network thread waits on it (with a timeout) and writes the reply line.
 * Event-API calls ("event", "teleport") reply through a temporary LLEventStream that lives
 * in a main-thread-only registry, so pumps are only ever created and destroyed there.
 */

#include "llviewerprecompiledheaders.h"

#if SL_TESTCTL

#include "sltestctl.h"

#include "llagent.h"
#include "llagentcamera.h"
#include "llappviewer.h"
#include "llcontrol.h"
#include "lldir.h"
#include "llevents.h"
#include "llsdjson.h"
#include "llsdutil.h"
#include "llstartup.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "workqueue.h"

#include <boost/json.hpp>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>

namespace
{
    constexpr int DEFAULT_PORT = 47800;
    constexpr F64 DEFAULT_TIMEOUT_SEC = 30.0;
    constexpr size_t MAX_LINE = 1 << 20;

    // One in-flight command. Filled once on the main thread, read by the network thread.
    struct Pending
    {
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
        LLSD reply;

        void finish(const LLSD& r)
        {
            {
                std::lock_guard<std::mutex> lock(mutex);
                if (done)
                {
                    return;
                }
                reply = r;
                done = true;
            }
            cv.notify_all();
        }
    };

    std::atomic<bool> sStop{ false };
    std::thread sThread;
    int sListenFd = -1;
    std::string sToken;

    // Main thread only: reply pumps for event-API calls, keyed by request id.
    struct ReplyPump
    {
        std::unique_ptr<LLEventStream> pump;
        LLTempBoundListener conn;
    };
    std::map<U64, ReplyPump> sReplyPumps;
    U64 sNextId = 1;   // network thread only

    LLSD err(const std::string& msg)
    {
        return llsd::map("ok", false, "error", msg);
    }

    LLSD vec3(const LLVector3& v)
    {
        return llsd::array(v.mV[VX], v.mV[VY], v.mV[VZ]);
    }

    LLVector3 toVec3(const LLSD& a)
    {
        return LLVector3((F32)a[0].asReal(), (F32)a[1].asReal(), (F32)a[2].asReal());
    }

    // Constant-time compare so the token can't be probed byte by byte.
    bool tokenMatches(const std::string& given)
    {
        if (given.size() != sToken.size() || sToken.empty())
        {
            return false;
        }
        unsigned char diff = 0;
        for (size_t i = 0; i < given.size(); ++i)
        {
            diff |= (unsigned char)(given[i] ^ sToken[i]);
        }
        return diff == 0;
    }

    std::string loadOrCreateToken()
    {
        const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_USER_SETTINGS, "testctl.token");
        {
            std::ifstream in(path);
            std::string tok;
            if (in && std::getline(in, tok) && tok.size() >= 32)
            {
                return tok;
            }
        }
        std::random_device rd;
        static const char* hex = "0123456789abcdef";
        std::string tok;
        for (int i = 0; i < 64; ++i)
        {
            tok += hex[rd() & 15];
        }
        ::unlink(path.c_str());
        const int fd = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd < 0)
        {
            LL_WARNS("TestCtl") << "cannot write " << path << LL_ENDL;
            return std::string();
        }
        const std::string line = tok + "\n";
        const bool wrote = ::write(fd, line.data(), line.size()) == (ssize_t)line.size();
        ::close(fd);
        return wrote ? tok : std::string();
    }

    // ---- commands (main thread) ----

    LLSD cmdStatus()
    {
        LLSD r = llsd::map("ok", true);
        r["state"] = LLStartUp::getStartupStateString();
        r["state_id"] = (S32)LLStartUp::getStartupState();
        r["logged_in"] = LLStartUp::getStartupState() >= STATE_STARTED;
        r["quitting"] = LLApp::isExiting();
        r["version"] = LLVersionInfo::instance().getChannelAndVersion();
        r["pid"] = (S32)::getpid();
        const F32 dt = gFrameIntervalSeconds.value();
        r["frame_ms"] = dt * 1000.f;
        r["fps"] = dt > 0.f ? 1.f / dt : 0.f;
        if (gViewerWindow)
        {
            r["window"] = llsd::array(gViewerWindow->getWindowWidthRaw(), gViewerWindow->getWindowHeightRaw());
        }
        if (LLViewerRegion* region = gAgent.getRegion())
        {
            r["region"] = region->getName();
            r["pos"] = vec3(gAgent.getPositionAgent() - region->getOriginAgent());
            r["camera"] = vec3(gAgentCamera.getCameraPositionAgent() - region->getOriginAgent());
        }
        return r;
    }

    LLSD cmdGet(const LLSD& req)
    {
        LLControlVariable* control = gSavedSettings.getControl(req["name"].asString());
        if (!control)
        {
            return err("no such setting");
        }
        return llsd::map("ok", true, "value", control->getValue());
    }

    LLSD cmdSet(const LLSD& req)
    {
        LLControlVariable* control = gSavedSettings.getControl(req["name"].asString());
        if (!control)
        {
            return err("no such setting");
        }
        control->setValue(req["value"]);
        return llsd::map("ok", true, "value", control->getValue());
    }

    LLSD cmdCamera(const LLSD& req)
    {
        if (req["reset"].asBoolean())
        {
            gAgentCamera.resetView();
            return llsd::map("ok", true);
        }
        LLViewerRegion* region = gAgent.getRegion();
        if (!region)
        {
            return err("no region");
        }
        if (!req.has("pos") || !req.has("focus"))
        {
            return err("need pos and focus (region coords) or reset");
        }
        gAgentCamera.setCameraPosAndFocusGlobal(region->getPosGlobalFromRegion(toVec3(req["pos"])),
                                                region->getPosGlobalFromRegion(toVec3(req["focus"])), LLUUID::null);
        return llsd::map("ok", true);
    }

    LLSD cmdSnapshot(const LLSD& req)
    {
        if (!gViewerWindow)
        {
            return err("no window");
        }
        std::string path = req["path"].asString();
        if (path.empty())
        {
            path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, "testctl_snapshot.png");
        }
        const S32 w = req.has("width") ? req["width"].asInteger() : gViewerWindow->getWindowWidthRaw();
        const S32 h = req.has("height") ? req["height"].asInteger() : gViewerWindow->getWindowHeightRaw();
        const bool show_ui = req.has("showui") ? req["showui"].asBoolean() : true;
        const bool show_hud = req.has("showhud") ? req["showhud"].asBoolean() : true;
        // saveSnapshot defaults to BMP; pick the encoder from the extension.
        const std::string ext = gDirUtilp->getExtension(path);
        const LLSnapshotModel::ESnapshotFormat format = (ext == "jpg" || ext == "jpeg") ? LLSnapshotModel::SNAPSHOT_FORMAT_JPEG
                                                       : (ext == "bmp") ? LLSnapshotModel::SNAPSHOT_FORMAT_BMP
                                                                        : LLSnapshotModel::SNAPSHOT_FORMAT_PNG;
        const bool ok = gViewerWindow->saveSnapshot(path, w, h, show_ui, show_hud, false, true,
                                                    LLSnapshotModel::SNAPSHOT_TYPE_COLOR, format);
        return llsd::map("ok", ok, "path", path);
    }

    // Post `data` to the named event pump with a temporary reply pump; the first event on it
    // finishes the command. Ops that never reply finish after "wait" seconds (network side).
    void cmdEvent(U64 id, const std::string& pump_name, LLSD data, const std::shared_ptr<Pending>& p)
    {
        ReplyPump& rp = sReplyPumps[id];
        rp.pump = std::make_unique<LLEventStream>("testctl_reply", true);
        rp.conn = rp.pump->listen("testctl", [p](const LLSD& event)
        {
            p->finish(llsd::map("ok", true, "reply", event));
            return false;
        });
        if (data.isUndefined())
        {
            data = LLSD::emptyMap();
        }
        data["reply"] = rp.pump->getName();
        LLEventPumps::instance().obtain(pump_name).post(data);
    }

    void runCommand(U64 id, const LLSD& req, const std::shared_ptr<Pending>& p)
    {
        const std::string cmd = req["cmd"].asString();
        if (cmd == "ping")
        {
            p->finish(llsd::map("ok", true, "pong", true));
        }
        else if (cmd == "status")
        {
            p->finish(cmdStatus());
        }
        else if (cmd == "login")
        {
            if (LLStartUp::getStartupState() != STATE_LOGIN_WAIT)
            {
                p->finish(err("not at the login screen (state " + LLStartUp::getStartupStateString() + ")"));
                return;
            }
            LLEventPumps::instance().obtain("LLPanelLogin").post(llsd::map("op", "onClickConnect"));
            p->finish(llsd::map("ok", true));
        }
        else if (cmd == "quit")
        {
            p->finish(llsd::map("ok", true));
            LLAppViewer::instance()->requestQuit();
        }
        else if (cmd == "forcequit")
        {
            p->finish(llsd::map("ok", true));
            LLAppViewer::instance()->forceQuit();
        }
        else if (cmd == "get")
        {
            p->finish(cmdGet(req));
        }
        else if (cmd == "set")
        {
            p->finish(cmdSet(req));
        }
        else if (cmd == "camera")
        {
            p->finish(cmdCamera(req));
        }
        else if (cmd == "snapshot")
        {
            p->finish(cmdSnapshot(req));
        }
        else if (cmd == "log")
        {
            LL_INFOS("TestCtl") << req["text"].asString() << LL_ENDL;
            p->finish(llsd::map("ok", true));
        }
        else if (cmd == "teleport")
        {
            if (LLStartUp::getStartupState() < STATE_STARTED)
            {
                p->finish(err("not logged in"));
                return;
            }
            LLSD data = llsd::map("op", "requestTeleport", "regionname", req["region"],
                                  "x", req["x"], "y", req["y"], "z", req["z"], "skip_confirmation", true);
            LLEventPumps::instance().obtain("LLAgent").post(data);
            p->finish(llsd::map("ok", true));
        }
        else if (cmd == "event")
        {
            const std::string pump = req["pump"].asString();
            if (pump.empty())
            {
                p->finish(err("need pump"));
                return;
            }
            cmdEvent(id, pump, req["data"], p);
        }
        else
        {
            p->finish(err("unknown cmd '" + cmd + "'"));
        }
    }

    // ---- network thread ----

    bool sendLine(int fd, const LLSD& reply)
    {
        std::string line = boost::json::serialize(LlsdToJson(reply));
        line += '\n';
        size_t off = 0;
        while (off < line.size())
        {
            const ssize_t n = ::send(fd, line.data() + off, line.size() - off, MSG_NOSIGNAL);
            if (n <= 0)
            {
                return false;
            }
            off += (size_t)n;
        }
        return true;
    }

    LLSD execute(const LLSD& req)
    {
        LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
        if (!main_queue)
        {
            return err("main loop gone");
        }
        const U64 id = sNextId++;
        auto p = std::make_shared<Pending>();
        if (!main_queue->post([id, req, p]() { runCommand(id, req, p); }))
        {
            return err("main loop closed");
        }

        // "wait" bounds event-API calls that never reply; everything else should finish
        // within a frame or two (the main thread may be in a long hitch, hence the margin).
        const bool is_event = req["cmd"].asString() == "event";
        const F64 limit = req.has("wait") ? req["wait"].asReal() : (is_event ? 5.0 : DEFAULT_TIMEOUT_SEC);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<F64>(limit);
        LLSD reply;
        {
            std::unique_lock<std::mutex> lock(p->mutex);
            while (!p->done && !sStop && std::chrono::steady_clock::now() < deadline)
            {
                p->cv.wait_for(lock, std::chrono::milliseconds(250));
            }
            if (p->done)
            {
                reply = p->reply;
            }
            else if (is_event)
            {
                reply = llsd::map("ok", true, "reply", LLSD(), "note", "no reply within wait");
            }
            else
            {
                reply = err("timed out waiting for the main thread");
            }
        }
        if (is_event)
        {
            main_queue->post([id]() { sReplyPumps.erase(id); });
        }
        return reply;
    }

    void serveClient(int fd)
    {
        std::string buf;
        bool authed = false;
        char chunk[4096];
        while (!sStop)
        {
            pollfd pfd{ fd, POLLIN, 0 };
            const int pr = ::poll(&pfd, 1, 250);
            if (pr < 0)
            {
                break;
            }
            if (pr == 0)
            {
                continue;
            }
            const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n <= 0)
            {
                break;
            }
            buf.append(chunk, (size_t)n);
            if (buf.size() > MAX_LINE)
            {
                break;
            }
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos)
            {
                const std::string line = buf.substr(0, nl);
                buf.erase(0, nl + 1);
                if (line.empty())
                {
                    continue;
                }
                boost::system::error_code ec;
                const boost::json::value jv = boost::json::parse(line, ec);
                if (ec)
                {
                    if (!sendLine(fd, err("bad json")))
                    {
                        return;
                    }
                    continue;
                }
                const LLSD req = LlsdFromJson(jv);
                if (!authed)
                {
                    if (!tokenMatches(req["token"].asString()))
                    {
                        LL_WARNS("TestCtl") << "rejected a connection with a bad token" << LL_ENDL;
                        sendLine(fd, err("bad token"));
                        return;
                    }
                    authed = true;
                    if (!sendLine(fd, llsd::map("ok", true, "hello", LLVersionInfo::instance().getChannelAndVersion())))
                    {
                        return;
                    }
                    continue;
                }
                if (!sendLine(fd, execute(req)))
                {
                    return;
                }
            }
        }
    }

    void threadMain()
    {
        LL_INFOS("TestCtl") << "listening" << LL_ENDL;
        while (!sStop)
        {
            pollfd pfd{ sListenFd, POLLIN, 0 };
            if (::poll(&pfd, 1, 250) <= 0)
            {
                continue;
            }
            const int fd = ::accept(sListenFd, nullptr, nullptr);
            if (fd < 0)
            {
                continue;
            }
            serveClient(fd);
            ::close(fd);
        }
    }
}

void SLTestCtl::start()
{
    const char* enabled = std::getenv("SLIPSTREAM_TESTCTL");
    if (enabled && std::string(enabled) == "0")
    {
        LL_INFOS("TestCtl") << "disabled by SLIPSTREAM_TESTCTL=0" << LL_ENDL;
        return;
    }
    int port = DEFAULT_PORT;
    if (const char* p = std::getenv("SLIPSTREAM_TESTCTL_PORT"))
    {
        port = std::atoi(p);
    }
    sToken = loadOrCreateToken();
    if (sToken.empty())
    {
        LL_WARNS("TestCtl") << "no token, not starting" << LL_ENDL;
        return;
    }

    sListenFd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sListenFd < 0)
    {
        return;
    }
    const int one = 1;
    ::setsockopt(sListenFd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);   // never reachable off the machine
    if (::bind(sListenFd, (sockaddr*)&addr, sizeof(addr)) != 0 || ::listen(sListenFd, 2) != 0)
    {
        LL_WARNS("TestCtl") << "cannot listen on 127.0.0.1:" << port << LL_ENDL;
        ::close(sListenFd);
        sListenFd = -1;
        return;
    }
    LL_INFOS("TestCtl") << "test control on 127.0.0.1:" << port << " (developer build)" << LL_ENDL;
    sStop = false;
    sThread = std::thread(threadMain);
}

void SLTestCtl::stop()
{
    if (sThread.joinable())
    {
        sStop = true;
        sThread.join();
    }
    if (sListenFd >= 0)
    {
        ::close(sListenFd);
        sListenFd = -1;
    }
    sReplyPumps.clear();
}

#endif // SL_TESTCTL
