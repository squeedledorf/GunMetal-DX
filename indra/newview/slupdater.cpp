/**
 * @file slupdater.cpp
 * @brief Auto-updater (#421): background download + verification of a signed update.
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
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldir.h"
#include "lldiriterator.h"
#include "lleventcoro.h"
#include "llfile.h"
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
    constexpr F32  CHUNK_PAUSE = 1.0f;               // seconds between chunks: stay out of the way
    constexpr S32  MAX_CHUNK_FAILURES = 4;

    bool sActive = false;                            // a download coroutine is running
    std::string sActiveBuild;
    std::string sStagedBuild;                        // staged this session

    // ---- state files -------------------------------------------------------------

    // Directory of the pointer / marker files. Linux: the launcher's XDG state dir (it reads
    // pending-update from there). Windows: %LOCALAPPDATA%\GunMetalDX\update (readable
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

    // Forget the staged update: the launcher / WinMain only act on this pointer file.
    void cancel_pending()
    {
        const std::string dir = state_dir();
        if (!dir.empty()) LLFile::remove(join(dir, "pending-update"));
        sStagedBuild.clear();
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

    // ---- download ----------------------------------------------------------------

    struct Job
    {
        SLUpdateApply::Manifest manifest;
        std::string             manifest_text;
        std::string             signature;
        std::string             key;           // linux_tarball / windows_setup
        std::string             version_label;
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
        const S32 code = http[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_STATUS].asInteger();
        const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http);
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

        auto finish = [&]() { sActive = false; sActiveBuild.clear(); };

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
            const U64 last = std::min<U64>(have + CHUNK_BYTES, art.size) - 1;
            std::string body;
            const S32 code = get_range(art.url, have, last, body);
            if (LLApp::isExiting()) { finish(); return; }
            if (code != 206 || body.size() != (size_t)(last - have + 1))
            {
                // 200 = the server ignored Range (would be the whole file); anything else =
                // network trouble. Keep the .part and try again at the next update check.
                LL_WARNS("SLUpdater") << "Chunk " << have << "-" << last << " failed: HTTP " << code
                                      << ", " << body.size() << " bytes" << LL_ENDL;
                if (++failures >= MAX_CHUNK_FAILURES || code == 200) { finish(); return; }
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
                    finish();
                    return;
                }
            }
            have += body.size();
            llcoro::suspendUntilTimeout(CHUNK_PAUSE);
        }
        if (have < art.size) { finish(); return; }   // exiting; resumes next run

        unsigned long long size = 0;
        const std::string digest = hash_in_thread(part, size);
        if (LLApp::isExiting()) { finish(); return; }
        if (size != art.size || !SLUpdateVerify::sameDigest(digest, art.sha256))
        {
            LL_WARNS("SLUpdater") << "Downloaded update does not match the signed manifest (sha256 " << digest
                                  << "); discarding it" << LL_ENDL;
            LLFile::remove(part);
            finish();
            return;
        }
        if (LLFile::rename(part, final_path) != 0)
        {
            finish();
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
            || !write_text_atomic(join(state_dir(), "pending-update"), staged + "\n"))
        {
            LL_WARNS("SLUpdater") << "Cannot record the staged update" << LL_ENDL;
            finish();
            return;
        }
        sStagedBuild = build;
        LL_INFOS("SLUpdater") << "Update " << build << " downloaded and verified; installs at the next start" << LL_ENDL;
        LLSD args;
        args["VERSION"] = job.version_label;
        // OK keeps the update pending; "Don't install" cancels it and keeps this build quiet.
        LLNotificationsUtil::add("UpdateStaged", args, LLSD(),
            [build](const LLSD& notification, const LLSD& response)
            {
                if (LLNotificationsUtil::getSelectedOption(notification, response) == 1)
                {
                    cancel_pending();
                    gSavedSettings.setString("UpdateCheckSkippedBuild", build);
                }
            });
        finish();
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

    bool onFeedUpdate(const std::string& manifest_text, const std::string& signature,
                      const std::string& feed_build, const std::string& current_build, bool release_feed)
    {
        if (!gSavedSettings.getBOOL("UpdateAutoInstall") || manifest_text.empty() || signature.empty()) return false;
        if (!supported()) return false;

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
        if (sStagedBuild == m.build) return true;
        if (sActive)
        {
            return sActiveBuild == m.build;
        }
        // Staged by an earlier run and still pending?
        std::string pointer;
        if (read_text(join(state_dir(), "pending-update"), pointer, 4096))
        {
            std::string rec;
            boost::system::error_code ec;
            if (read_text(trim_nl(pointer), rec))
            {
                const boost::json::value doc = boost::json::parse(rec, ec);
                if (!ec && doc.is_object())
                {
                    const boost::json::value* mt = doc.as_object().if_contains("manifest");
                    if (mt && mt->is_string() && std::string(mt->as_string()) == manifest_text) return true;
                }
            }
        }

        job.manifest_text = manifest_text;
        job.signature = signature;
        job.version_label = m.version;
        sActive = true;
        sActiveBuild = m.build;
        LLCoros::instance().launch("SLUpdater::download", [job]() { download_coro(job); });
        return true;
    }

    void confirmStartup()
    {
        // Turning UpdateAutoInstall off cancels an update that is downloaded but not yet
        // installed (the launcher applies whatever "pending-update" points at).
        static bool hooked = false;
        if (!hooked)
        {
            hooked = true;
            if (LLControlVariable* control = gSavedSettings.getControl("UpdateAutoInstall"))
            {
                control->getSignal()->connect([](LLControlVariable*, const LLSD& value, const LLSD&)
                {
                    if (!value.asBoolean()) cancel_pending();
                });
            }
        }
        const std::string dir = state_dir();
        if (dir.empty()) return;
        if (!gSavedSettings.getBOOL("UpdateAutoInstall"))
        {
            // Switched off since the download: don't install it behind the person's back.
            LLFile::remove(join(dir, "pending-update"));
        }
        const std::string marker = join(dir, "update-unconfirmed");
        if (LLFile::isfile(marker))
        {
            LL_INFOS("SLUpdater") << "Updated viewer reached the login screen; confirming the update" << LL_ENDL;
            LLFile::remove(marker);
        }
    }

    bool applyPendingAtStartup()
    {
#if LL_WINDOWS
        // Runs before almost anything is initialised: plain Win32 + std only.
        const std::string dir = state_dir();
        if (dir.empty()) return false;
        std::string pointer;
        if (!read_text(join(dir, "pending-update"), pointer, 4096)) return false;
        const std::string pending_file = join(dir, "pending-update");
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
