/**
 * @file slupdater.cpp
 * @brief Auto-updater (#421): download + verification of a signed update, on request.
 *
 * See slupdater.h for the flow. Everything that decides what is downloaded (URL, size,
 * sha256, version) comes from the signature-verified manifest (SLUpdateApply::parseManifest).
 */

#include "llviewerprecompiledheaders.h"

#include "slupdater.h"

#include "slupdateapply.h"
#include "slupdatechecker.h"
#include "slupdateverify.h"

#include <atomic>
#include <filesystem>
#include <fstream>
#include <thread>

#include <boost/json.hpp>

#include "llapp.h"
#include "llappviewer.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "lleventcoro.h"
#include "llfile.h"
#include "llfloater.h"
#include "llfloaterreg.h"
#include "llnotificationsutil.h"
#include "llstartup.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"

#if LL_WINDOWS
#   include <windows.h>
#endif

namespace
{
    constexpr U64  CHUNK_BYTES = 2u * 1024 * 1024;   // ranged requests: bounded memory, resumable
    constexpr S32  MAX_CHUNK_FAILURES = 4;

    // <GMDX> State files (state_dir()). Only APPROVED is ever installed at startup.
    constexpr const char* DOWNLOADED = "downloaded-update";   // downloaded + verified, not yet approved
    constexpr const char* APPROVED = "approved-update";       // the person chose to install it
    constexpr const char* LEGACY_PENDING = "pending-update";  // 0.3.0 and older, written unasked: = DOWNLOADED
    constexpr const char* DOWNLOAD_FLOATER = "gmdx_update_download";

    bool sActive = false;                            // a download coroutine is running
    bool sCancel = false;                            // the progress window's Cancel was clicked
    std::string sActiveBuild;
    std::string sPromptedBuild;                      // asked about this run (scheduled checks don't repeat it)

    // ---- state files -------------------------------------------------------------

    // Directory of the pointer / marker files. Linux: the launcher's XDG state dir (it reads
    // approved-update from there). Windows: %LOCALAPPDATA%\GunMetalDX\update (readable
    // before the viewer's own directories are set up).
    std::string state_dir()
    {
#if LL_WINDOWS
        const char* local = std::getenv("LOCALAPPDATA");
        const char* leaf = SLUpdateApply::isJetstreamBuild() ? "\\GunMetalDX\\update-jetstream" : "\\GunMetalDX\\update";
        return local && *local ? std::string(local) + leaf : std::string();
#else
        return SLUpdateApply::stateDir();
#endif
    }

    std::string join(const std::string& dir, const std::string& name)
    {
#if LL_WINDOWS
        return dir + "\\" + name;
#else
        return dir + "/" + name;
#endif
    }

    bool read_text(const std::string& path, std::string& out, size_t limit = 64 * 1024)
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return out.size() <= limit;
    }

    std::string trim_nl(std::string s)
    {
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
        return s;
    }

    bool write_text_atomic(const std::string& path, const std::string& text)
    {
        const std::string tmp = path + ".tmp";
        {
            std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
            if (!out) return false;
            out << text;
            if (!out.flush()) return false;
        }
        return LLFile::rename(tmp, path) == 0;
    }


    bool is_bad_build(const std::string& build)
    {
        std::string text;
        return read_text(join(state_dir(), "bad-build"), text, 256) && trim_nl(text) == build;
    }

    std::string updates_dir()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "updates");
    }

    // Linux: the install root is the launcher's directory = parent of bin/.
    std::string install_root()
    {
        return gDirUtilp->getExecutableDir() + "/..";
    }

    // Keep only the build being worked on: the cache never grows past one installer.
    void prune_updates(const std::string& keep_build)
    {
        const std::string dir = updates_dir();
        std::string name;
        LLDirIterator iter(dir, "*");
        while (iter.next(name))
        {
            if (name != keep_build) gDirUtilp->deleteDirAndContents(gDirUtilp->add(dir, name));
        }
    }

    // ---- downloaded updates and the person's answers ----------------------------------

    // A staged record ({manifest, signature, artifact, file}) a pointer file names, whose file
    // still exists; the manifest is signature-checked again.
    struct Staged
    {
        SLUpdateApply::Manifest manifest;
        std::string             manifest_text;
        std::string             artifact;
        std::string             file;
    };

    bool load_staged(const char* pointer_name, Staged& out)
    {
        const std::string dir = state_dir();
        std::string pointer, rec;
        if (dir.empty() || !read_text(join(dir, pointer_name), pointer, 4096) || !read_text(trim_nl(pointer), rec))
        {
            return false;
        }
        boost::system::error_code ec;
        const boost::json::value doc = boost::json::parse(rec, ec);
        if (ec || !doc.is_object()) return false;
        auto str = [&](const char* k) {
            const boost::json::value* v = doc.as_object().if_contains(k);
            return v && v->is_string() ? std::string(v->as_string()) : std::string();
        };
        out.manifest_text = str("manifest");
        out.artifact = str("artifact");
        out.file = str("file");
        std::string error;
        return LLFile::isfile(out.file)   // gone (cache cleared, pruned): nothing to install
               && SLUpdateApply::parseManifest(out.manifest_text, str("signature"), out.manifest, error);
    }

    // The downloaded update becomes the one to install at startup.
    bool approve()
    {
        const std::string dir = state_dir();
        if (dir.empty()) return false;
        const std::string approved = join(dir, APPROVED);
        if (LLFile::isfile(join(dir, DOWNLOADED)) && LLFile::replace(join(dir, DOWNLOADED), approved) != 0) return false;
        return LLFile::isfile(approved);
    }

    void discard(const std::string& build)
    {
        const std::string dir = state_dir();
        if (!dir.empty())
        {
            LLFile::remove(join(dir, DOWNLOADED), ENOENT);
            LLFile::remove(join(dir, APPROVED), ENOENT);
        }
        gDirUtilp->deleteDirAndContents(gDirUtilp->add(updates_dir(), build));   // build: four numbers (parseManifest)
        LL_INFOS("SLUpdater") << "Downloaded update " << build << " discarded" << LL_ENDL;
    }

#if LL_LINUX
    // The apply step runs in the launcher, so "Install now" starts the launcher again once this
    // process has gone (a detached sh polls our pid), with a clean SLIPSTREAM_UPDATE_APPLIED.
    bool start_launcher_after_exit()
    {
        const std::string launcher = install_root() + "/gunmetal-dx";
        if (access(launcher.c_str(), X_OK) != 0) return false;
        const std::string pid = std::to_string((long)getpid());
        long max_fd = sysconf(_SC_OPEN_MAX);
        if (max_fd <= 0 || max_fd > 65536) max_fd = 65536;
        const pid_t child = fork();
        if (child < 0) return false;
        if (child == 0)
        {
            // Child of a multi-threaded process: async-signal-safe calls only until exec.
            setsid();
            for (long fd = 3; fd < max_fd; ++fd) close((int)fd);
            execl("/bin/sh", "sh", "-c",
                  "while kill -0 \"$1\" 2>/dev/null; do sleep 1; done; unset SLIPSTREAM_UPDATE_APPLIED; exec \"$2\"",
                  "gmdx-update", pid.c_str(), launcher.c_str(), (char*)nullptr);
            _exit(127);
        }
        return true;
    }
#endif

    // Install now: approve, quit, and start again; the start installs it (launcher / WinMain).
    void install_now()
    {
        if (!approve())
        {
            LL_WARNS("SLUpdater") << "Cannot record the approved update" << LL_ENDL;
            LLNotificationsUtil::add("GMDXUpdateDownloadFailed", LLSD().with("REASON", "its record could not be written."));
            return;
        }
#if LL_LINUX
        if (!start_launcher_after_exit())
        {
            LLNotificationsUtil::add("GMDXUpdateInstallLater");
            return;
        }
        LL_INFOS("SLUpdater") << "Quitting to install the update" << LL_ENDL;
        LLAppViewer::instance()->requestQuit();
#else
        LLAppViewer::instance()->requestRelaunch({}, "to install the update");
#endif
    }

    // An update downloaded earlier (this run's prompts were answered "later", or 0.3.0 fetched it).
    void ask_downloaded(const std::string& build, const std::string& label)
    {
        sPromptedBuild = build;
        LLNotificationsUtil::add("GMDXUpdateDownloaded", LLSD().with("VERSION", label), LLSD(),
            [build](const LLSD& notification, const LLSD& response)
            {
                switch (LLNotificationsUtil::getSelectedOption(notification, response))
                {
                    case 0: install_now(); break;
                    case 2: discard(build); break;
                    default: break;   // Later: asked again at the next start
                }
            });
    }

    // ---- download progress window -----------------------------------------------------

    class GMDXFloaterUpdateDownload : public LLFloater
    {
    public:
        GMDXFloaterUpdateDownload(const LLSD& key) : LLFloater(key) {}
        bool postBuild() override
        {
            getChild<LLUICtrl>("cancel_btn")->setCommitCallback([](LLUICtrl*, const LLSD&) { sCancel = true; });
            return true;
        }
    };

    // Closing the window only hides it: the download carries on and still asks at the end.
    void show_progress(const std::string& label, U64 have, U64 total, bool verifying)
    {
        LLFloater* f = LLFloaterReg::findInstance(DOWNLOAD_FLOATER);
        if (!f || !f->getVisible()) return;
        LLStringUtil::format_map_t args;
        args["[VERSION]"] = label;
        args["[DONE]"] = llformat("%.1f", (F64)have / (1024.0 * 1024.0));
        args["[TOTAL]"] = llformat("%.1f", (F64)total / (1024.0 * 1024.0));
        f->getChild<LLUICtrl>("status")->setValue(f->getString(verifying ? "verifying" : "downloading", args));
        f->getChild<LLUICtrl>("progress")->setValue(total ? 100.0 * (F64)have / (F64)total : 0.0);
    }

    // ---- download ----------------------------------------------------------------

    struct Job
    {
        SLUpdateApply::Manifest manifest;
        std::string             manifest_text;
        std::string             signature;
        std::string             key;           // linux_tarball / windows_setup
        std::string             version_label;   // the feed's label, shown in the prompts
    };

    // GET one byte range. Returns the HTTP status; `body` receives the data on 206.
    S32 get_range(const std::string& url, U64 first, U64 last, std::string& body)
    {
        LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter(new LLCoreHttpUtil::HttpCoroutineAdapter("UpdateDownload", policy));
        LLCore::HttpRequest::ptr_t request(new LLCore::HttpRequest);
        LLCore::HttpOptions::ptr_t options(new LLCore::HttpOptions);
        options->setTimeout(60);
        options->setRetries(0);
        options->setFollowRedirects(true);
        LLCore::HttpHeaders::ptr_t headers(new LLCore::HttpHeaders);
        headers->append("Range", llformat("bytes=%llu-%llu", (unsigned long long)first, (unsigned long long)last));
        LLSD result = adapter->getRawAndSuspend(request, url, options, headers);
        const LLSD& http = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http);
        // <GMDX> The HTTP code is the status's TYPE; HTTP_RESULTS_STATUS is LLCore's error code (0 on
        // success), so every good 206 read as "HTTP 0" and no update ever downloaded.
        const S32 code = (S32)status.getType();
        if (status && code == 206)
        {
            const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
            body.assign(raw.begin(), raw.end());
        }
        return code;
    }

    // SHA-256 of a big file off the main thread; the coroutine polls for completion.
    std::string hash_in_thread(const std::string& path, unsigned long long& size)
    {
        std::atomic<bool> done(false);
        std::string digest;
        unsigned long long bytes = 0;
        std::thread worker([&]()
        {
            digest = SLUpdateVerify::sha256File(path, &bytes);
            done = true;
        });
        while (!done && !LLApp::isExiting())
        {
            llcoro::suspendUntilTimeout(0.1f);
        }
        worker.join();
        size = bytes;
        return digest;
    }

    void download_coro(Job job)
    {
        const SLUpdateApply::Artifact& art = job.manifest.artifacts.at(job.key);
        const std::string build = job.manifest.build;
        const std::string dir = gDirUtilp->add(updates_dir(), build);
        LLFile::mkdir(updates_dir());
        prune_updates(build);
        LLFile::mkdir(dir);
        const std::string part = gDirUtilp->add(dir, art.name + ".part");
        const std::string final_path = gDirUtilp->add(dir, art.name);

        const std::string label = job.version_label;
        auto finish = [&]() { sActive = false; sActiveBuild.clear(); LLFloaterReg::hideInstance(DOWNLOAD_FLOATER); };
        // <GMDX> the person is waiting: every failure is told
        auto fail = [&](const std::string& reason)
        {
            finish();
            LLNotificationsUtil::add("GMDXUpdateDownloadFailed", LLSD().with("REASON", reason));
        };

        // Resume: the .part file is a prefix of the artifact. A leftover that is too big, or
        // from a different size, is discarded.
        U64 have = 0;
        if (LLFile::isfile(part))
        {
            llstat st; have = LLFile::stat(part, &st) == 0 ? (U64)st.st_size : 0;
            if (have > art.size) { LLFile::remove(part); have = 0; }
        }
        LL_INFOS("SLUpdater") << "Downloading " << art.name << " (" << art.size << " bytes), resuming at " << have << LL_ENDL;

        S32 failures = 0;
        while (have < art.size && !LLApp::isExiting())
        {
            show_progress(label, have, art.size, false);
            if (sCancel)
            {
                LL_INFOS("SLUpdater") << "Download cancelled at " << have << " bytes; kept for a later click" << LL_ENDL;
                finish();
                return;
            }
            const U64 last = std::min<U64>(have + CHUNK_BYTES, art.size) - 1;
            std::string body;
            const S32 code = get_range(art.url, have, last, body);
            if (LLApp::isExiting()) { finish(); return; }
            if (code != 206 || body.size() != (size_t)(last - have + 1))
            {
                // 200 = the server ignored Range (would be the whole file); anything else =
                // network trouble. Keep the .part: the next "Download and install" resumes it.
                LL_WARNS("SLUpdater") << "Chunk " << have << "-" << last << " failed: HTTP " << code
                                      << ", " << body.size() << " bytes" << LL_ENDL;
                if (++failures >= MAX_CHUNK_FAILURES || code == 200)
                {
                    fail(llformat("the server answered HTTP %d.", code));
                    return;
                }
                llcoro::suspendUntilTimeout(5.f * failures);
                continue;
            }
            failures = 0;
            {
                std::ofstream out(part, std::ios::binary | std::ios::app);
                out.write(body.data(), (std::streamsize)body.size());
                if (!out.flush())
                {
                    LL_WARNS("SLUpdater") << "Cannot write " << part << " (disk full?)" << LL_ENDL;
                    fail("it could not be saved (is the disk full?).");
                    return;
                }
            }
            have += body.size();
            llcoro::suspend();   // <GMDX> the person is waiting for it: no pause between chunks
        }
        if (have < art.size) { finish(); return; }   // exiting; resumes after the next click

        show_progress(label, have, art.size, true);
        unsigned long long size = 0;
        const std::string digest = hash_in_thread(part, size);
        if (LLApp::isExiting()) { finish(); return; }
        if (size != art.size || !SLUpdateVerify::sameDigest(digest, art.sha256))
        {
            LL_WARNS("SLUpdater") << "Downloaded update does not match the signed manifest (sha256 " << digest
                                  << "); discarding it" << LL_ENDL;
            LLFile::remove(part);
            fail("the downloaded file did not match its signature, so it was deleted.");
            return;
        }
        if (LLFile::rename(part, final_path) != 0)
        {
            fail("it could not be saved.");
            return;
        }

        // The staged record: the signed manifest travels with the file, so the apply step
        // (a separate process, later) verifies everything again from scratch.
        boost::json::object rec;
        rec["manifest"] = job.manifest_text;
        rec["signature"] = job.signature;
        rec["artifact"] = job.key;
        rec["file"] = final_path;
        const std::string staged = gDirUtilp->add(dir, "staged.json");
        { std::error_code mk_ec; std::filesystem::create_directories(state_dir(), mk_ec); }
        if (!write_text_atomic(staged, boost::json::serialize(rec))
            || !write_text_atomic(join(state_dir(), DOWNLOADED), staged + "\n"))
        {
            LL_WARNS("SLUpdater") << "Cannot record the staged update" << LL_ENDL;
            fail("its record could not be written.");
            return;
        }
        LL_INFOS("SLUpdater") << "Update " << build << " downloaded and verified; asking when to install it" << LL_ENDL;
        finish();
        // <GMDX> Install now / Install at next start. Closed unanswered: asked again at the next start.
        sPromptedBuild = build;
        LLNotificationsUtil::add("GMDXUpdateReady", LLSD().with("VERSION", label), LLSD(),
            [](const LLSD& notification, const LLSD& response)
            {
                switch (LLNotificationsUtil::getSelectedOption(notification, response))
                {
                    case 0: install_now(); break;
                    case 1: approve(); break;
                    default: break;
                }
            });
    }

    void start_download(const Job& job)
    {
        if (sActive) return;
        sActive = true;
        sCancel = false;
        sActiveBuild = job.manifest.build;
        LLFloaterReg::showInstance(DOWNLOAD_FLOATER);
        LLCoros::instance().launch("SLUpdater::download", [job]() { download_coro(job); });
    }
}

namespace SLUpdater
{
    const char* artifactKey()
    {
        return SLUpdateChecker::installArtifactKey();
    }

    bool supported()
    {
        if (!SLUpdateVerify::keyConfigured() || !artifactKey()) return false;
#if LL_LINUX
        // The launcher swaps the install directory: the person must be able to write to it
        // and its parent. Otherwise they get the normal "download" notice.
        const std::string root = install_root();
        const std::string parent = root + "/..";
        return access(root.c_str(), W_OK | X_OK) == 0 && access(parent.c_str(), W_OK | X_OK) == 0
            && !LLFile::isfile(root + "/.packaged-by");
#else
        return true;
#endif
    }

    bool offerUpdate(const std::string& manifest_text, const std::string& signature,
                     const std::string& feed_build, const std::string& current_build, bool release_feed,
                     const std::string& label, const std::string& notes_url, bool modal, bool manual)
    {
        if (manifest_text.empty() || signature.empty() || !supported()) return false;

        Job job;
        std::string error;
        if (!SLUpdateApply::parseManifest(manifest_text, signature, job.manifest, error))
        {
            LL_WARNS("SLUpdater") << "Ignoring the update manifest: " << error << LL_ENDL;
            return false;
        }
        const SLUpdateApply::Manifest& m = job.manifest;
        // The unsigned feed fields must agree with the signed manifest; a mismatch means the
        // feed was tampered with or mis-assembled, so do nothing automatic.
        if (m.build != feed_build || (release_feed && m.channel != "release"))
        {
            LL_WARNS("SLUpdater") << "Signed manifest (" << m.build << ", " << m.channel
                                  << ") does not match the feed (" << feed_build << ")" << LL_ENDL;
            return false;
        }
        job.key = artifactKey();
        if (!m.artifacts.count(job.key)) return false;
        if (SLUpdateApply::compareBuild(m.build, current_build) != 1) return false;   // never downgrade / repeat
        if (is_bad_build(m.build))
        {
            LL_INFOS("SLUpdater") << "Build " << m.build << " failed to start on this machine before; not installing it" << LL_ENDL;
            return false;
        }
        const std::string build = m.build;
        if (sActive)
        {
            if (manual) LLFloaterReg::showInstance(DOWNLOAD_FLOATER);   // already downloading: show it
            return true;
        }
        if (!manual && sPromptedBuild == build) return true;            // asked about already this run
        // Downloaded earlier (maybe already approved): ask what to do with it, don't fetch again.
        Staged staged;
        if ((load_staged(DOWNLOADED, staged) || load_staged(APPROVED, staged)) && staged.manifest.build == build)
        {
            ask_downloaded(build, label);
            return true;
        }

        job.manifest_text = manifest_text;
        job.signature = signature;
        job.version_label = label;
        sPromptedBuild = build;
        LLSD args;
        args["VERSION"] = label;
        args["CURRENT"] = current_build;
        args["NOTES_URL"] = notes_url;
        LLNotificationsUtil::add(modal ? "GMDXUpdateOfferModal" : "GMDXUpdateOffer", args, LLSD(),
            [job](const LLSD& notification, const LLSD& response)
            {
                switch (LLNotificationsUtil::getSelectedOption(notification, response))
                {
                    case 0: start_download(job); break;
                    case 2: gSavedSettings.setString("UpdateCheckSkippedBuild", job.manifest.build); break;
                    default: break;   // Remind me later: offered again at the next start
                }
            });
        return true;
    }

    void askAboutDownloaded(const std::string& current_build)
    {
        static bool asked = false;
        if (asked) return;
        asked = true;
        const std::string dir = state_dir();
        if (dir.empty()) return;
        // 0.3.0 and older downloaded on their own and left pending-update for the launcher to
        // install; from now on that is only "downloaded": the person decides.
        const std::string legacy = join(dir, LEGACY_PENDING);
        if (LLFile::isfile(legacy))
        {
            if (LLFile::isfile(join(dir, DOWNLOADED))) LLFile::remove(legacy);
            else LLFile::rename(legacy, join(dir, DOWNLOADED));
        }
        if (!LLFile::isfile(join(dir, DOWNLOADED))) return;
        Staged s;
        const char* key = artifactKey();
        llstat st;
        const bool usable = supported() && load_staged(DOWNLOADED, s) && key && s.artifact == key
                            && s.manifest.artifacts.count(key)
                            && SLUpdateApply::compareBuild(s.manifest.build, current_build) == 1
                            && !is_bad_build(s.manifest.build) && LLFile::stat(s.file, &st) == 0
                            && (U64)st.st_size == s.manifest.artifacts.at(key).size;
        if (!usable)
        {
            LL_INFOS("SLUpdater") << "Dropping a downloaded update that no longer applies" << LL_ENDL;
            LLFile::remove(join(dir, DOWNLOADED));
            return;
        }
        ask_downloaded(s.manifest.build, s.manifest.version);
    }

    void confirmStartup()
    {
        const std::string dir = state_dir();
        if (dir.empty()) return;
        const std::string marker = join(dir, "update-unconfirmed");
        if (LLFile::isfile(marker))
        {
            LL_INFOS("SLUpdater") << "Updated viewer reached the login screen; confirming the update" << LL_ENDL;
            LLFile::remove(marker);
        }
    }

    LLFloater* buildDownloadFloater(const LLSD& key)
    {
        return new GMDXFloaterUpdateDownload(key);
    }

    bool applyPendingAtStartup()
    {
#if LL_WINDOWS
        // Runs before almost anything is initialised: plain Win32 + std only.
        const std::string dir = state_dir();
        if (dir.empty()) return false;
        // <GMDX> Only an update the person chose to install (approved-update); a merely downloaded
        // one (downloaded-update, or 0.3.0's pending-update) waits for their answer in the viewer.
        std::string pointer;
        const std::string pending_file = join(dir, "approved-update");
        if (!read_text(pending_file, pointer, 4096)) return false;
        auto drop = [&]() { LLFile::remove(pending_file); };

        std::string rec;
        boost::system::error_code ec;
        if (!read_text(trim_nl(pointer), rec)) { drop(); return false; }
        const boost::json::value doc = boost::json::parse(rec, ec);
        if (ec || !doc.is_object()) { drop(); return false; }
        auto str = [&](const char* k) {
            const boost::json::value* v = doc.as_object().if_contains(k);
            return v && v->is_string() ? std::string(v->as_string()) : std::string();
        };
        SLUpdateApply::Manifest m;
        std::string error;
        if (!SLUpdateApply::parseManifest(str("manifest"), str("signature"), m, error)) { drop(); return false; }
        auto art = m.artifacts.find("windows_setup");
        const std::string file = str("file");
        const LLVersionInfo& info = LLVersionInfo::instance();
        const std::string running = llformat("%d.%d.%d.%d", info.getMajor(), info.getMinor(), info.getPatch(), (S32)info.getBuild());
        if (art == m.artifacts.end() || str("artifact") != "windows_setup" || file.empty()
            || SLUpdateApply::compareBuild(m.build, running) != 1 || is_bad_build(m.build))
        {
            drop();
            return false;
        }
        unsigned long long size = 0;
        const std::string digest = SLUpdateVerify::sha256File(file, &size);
        if (size != art->second.size || !SLUpdateVerify::sameDigest(digest, art->second.sha256)) { drop(); return false; }

        // One attempt per build: if the previous attempt never reached the login screen,
        // give up on this build instead of looping.
        const std::string marker = join(dir, "update-unconfirmed");
        std::string previous;
        if (read_text(marker, previous, 256) && trim_nl(previous).rfind(m.build, 0) == 0)
        {
            write_text_atomic(join(dir, "bad-build"), m.build + "\n");
            LLFile::remove(marker);
            drop();
            return false;
        }
        write_text_atomic(marker, m.build + "\n" + running + "\n");
        drop();

        // Hand over to the installer from a separate cmd.exe so this process can exit first:
        // the installer refuses to run while GunMetalDX.exe is listed. /S = silent, it
        // reuses the existing install directory (InstallDirRegKey). The viewer is started
        // again afterwards whatever the installer's exit code.
        wchar_t exe[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, exe, (DWORD)(sizeof(exe) / sizeof(exe[0])));
        const std::wstring wfile = ll_convert_string_to_wide(file);
        std::wstring cmd = L"cmd.exe /d /s /c \"ping -n 4 127.0.0.1 >nul & \"" + wfile + L"\" /S & \"" + exe + L"\"\"";
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr,
                            nullptr, &si, &pi))
        {
            LLFile::remove(marker);
            return false;
        }
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
        return true;
#else
        return false;
#endif
    }
}
