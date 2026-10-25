/**
 * @file slrevocationupdater.cpp
 * @brief Loading, refreshing and distributing the certificate revocation data.
 *        See slrevocationupdater.h.
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

#include "slrevocationupdater.h"

#include "slrevocation.h"

#include <atomic>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <functional>
#include <thread>

#include <openssl/sha.h>

#include "llapp.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldir.h"
#include "lleventcoro.h"
#include "llfile.h"
#include "lltimer.h"
#include "llviewercontrol.h"
#include "llviewermedia.h"

namespace
{
    constexpr double FIRST_FETCH_DELAY = 10.0;           // seconds after startup
    constexpr double REFRESH_INTERVAL  = 6.0 * 3600.0;   // Chrome checks components about as often
    constexpr double RETRY_INTERVAL    = 30.0 * 60.0;    // after a failed fetch
    constexpr size_t MAX_REPLY_BYTES   = 256u * 1024;    // Omaha reply
    constexpr size_t MAX_CRX_BYTES     = 16u * 1024 * 1024;
    constexpr size_t MAX_FILE_BYTES    = 32u * 1024 * 1024;

    // Main thread only (init, idle and the fetch coroutine all run there).
    bool        sInitialized = false;
    bool        sFetching = false;
    double      sNextFetch = 0.0;      // LLTimer::getTotalSeconds() of the next fetch
    uint32_t    sSequence = 0;         // sequence of the CRLSet in force (0 = none)
    std::string sBrowserPath;          // file holding that CRLSet

    std::string shipped_path(const char* name)
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, "revocation", name);
    }

    std::string cache_dir()
    {
        return gDirUtilp->getExpandedFilename(LL_PATH_CACHE, "revocation");
    }

    std::string cache_path()
    {
        return gDirUtilp->add(cache_dir(), "crl-set");
    }

    bool read_file(const std::string& path, std::string& out, size_t limit)
    {
        llifstream in(path.c_str(), std::ios::in | std::ios::binary);
        if (!in.is_open()) return false;
        out.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        return out.size() <= limit;
    }

    bool write_file_atomic(const std::string& path, const std::string& data)
    {
        const std::string tmp = path + ".tmp";
        {
            llofstream out(tmp.c_str(), std::ios::out | std::ios::binary | std::ios::trunc);
            if (!out.is_open()) return false;
            out.write(data.data(), std::streamsize(data.size()));
            if (!out.flush()) return false;
        }
        if (LLFile::replace(tmp, path) != 0)   // atomic, also over an existing file on Windows
        {
            LLFile::remove(tmp);
            return false;
        }
        return true;
    }

    std::shared_ptr<const SLRevocation::CRLSet> load_crlset(const std::string& path, std::string& error)
    {
        std::string data;
        if (!read_file(path, data, MAX_FILE_BYTES))
        {
            error = "not readable";
            return nullptr;
        }
        return SLRevocation::CRLSet::parse(data, &error);
    }

    std::string utc(uint64_t t)
    {
        if (!t) return "none";
        char buf[32];
        const std::time_t tt = std::time_t(t);
        std::tm tm_utc{};
#if LL_WINDOWS
        gmtime_s(&tm_utc, &tt);
#else
        gmtime_r(&tt, &tm_utc);
#endif
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M UTC", &tm_utc);
        return buf;
    }

    // "fetched 2.3 h ago" from the file's modification time.
    std::string file_age(const std::string& path)
    {
        llstat st;
        if (LLFile::stat(path, &st) != 0) return "age unknown";
        const double hours = double(std::time(nullptr) - st.st_mtime) / 3600.0;
        return llformat("file %.1f h old", hours);
    }

    std::string describe_crlset(const SLRevocation::CRLSet& set, const std::string& source,
                                const std::string& path)
    {
        const std::time_t now = std::time(nullptr);
        std::string validity;
        if (set.notAfter())
        {
            const double hours = (double(set.notAfter()) - double(now)) / 3600.0;
            validity = hours >= 0 ? llformat("NotAfter %s, %.1f h left", utc(set.notAfter()).c_str(), hours)
                                  : llformat("NotAfter %s, EXPIRED %.1f h ago (revocations in it still apply)",
                                             utc(set.notAfter()).c_str(), -hours);
        }
        else
        {
            validity = "no NotAfter";
        }
        return llformat("CRLSet sequence %u from %s (%s; %s): %zu blocked keys, %zu issuers with %zu revoked serials, %zu limited subjects",
                        set.sequence(), source.c_str(), file_age(path).c_str(), validity.c_str(),
                        set.blockedSPKICount(), set.parentCount(), set.serialCount(), set.limitedSubjectCount());
    }

    void hand_to_browser(const std::string& path)
    {
        if (LLViewerMedia::instanceExists())
        {
            LLViewerMedia::getInstance()->setCRLSetPath(path);
        }
    }

    // Runs `work` on a worker thread and suspends this coroutine until it is done, so the
    // signature check, inflate and parse of a ~1.2 MB CRLSet never stall a frame.
    void run_off_main_thread(const std::function<void()>& work)
    {
        std::atomic<bool> done(false);
        std::thread worker([&]()
        {
            work();
            done = true;
        });
        try
        {
            while (!done)
            {
                llcoro::suspendUntilTimeout(0.05f);
            }
        }
        catch (...)
        {
            // Coroutine shutdown: the worker only touches this frame's locals, so it must
            // finish before they go away. It is CPU-only and short.
            worker.join();
            throw;
        }
        worker.join();
    }

    struct GetResult
    {
        S32 code = 0;
        bool ok = false;
        std::string body;
        std::string error;
    };

    GetResult http_get(const std::string& url, size_t max_bytes)
    {
        LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter(
            new LLCoreHttpUtil::HttpCoroutineAdapter("CRLSetFetch", policy));
        LLCore::HttpRequest::ptr_t request(new LLCore::HttpRequest);
        LLCore::HttpOptions::ptr_t options(new LLCore::HttpOptions);
        options->setTimeout(120);
        options->setRetries(1);
        options->setFollowRedirects(true);
        options->setMaxBodyBytes(max_bytes);
        // The CRX URL comes out of a network reply: never let it reach the LAN.
        options->setBlockPrivateAddresses(true);
        LLSD result = adapter->getRawAndSuspend(request, url, options);

        GetResult out;
        const LLSD& http = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http);
        // The HTTP code is the status's type; HTTP_RESULTS_STATUS holds the low-level status (0 on success).
        out.code = (S32)status.getType();
        if (status && out.code == 200)
        {
            const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
            out.body.assign(raw.begin(), raw.end());
            out.ok = out.body.size() <= max_bytes;
            if (!out.ok) out.error = "reply too large";
        }
        else
        {
            out.error = status.toString();
        }
        return out;
    }

    std::string sha256_hex(const std::string& data)
    {
        unsigned char md[SHA256_DIGEST_LENGTH];
        SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), md);
        return SLRevocation::toHex(std::string_view(reinterpret_cast<const char*>(md), sizeof(md)));
    }

    // One fetch. Returns true when the CRLSet in force is current afterwards (updated, or
    // Omaha says there is nothing newer); false on any failure (retried sooner).
    bool fetch_once()
    {
        const std::string omaha_url = SLRevocation::omahaRequestURL(sSequence, true);
        GetResult reply = http_get(omaha_url, MAX_REPLY_BYTES);
        if (!reply.ok)
        {
            LL_WARNS("Revocation") << "CRLSet update check failed: HTTP " << reply.code << " " << reply.error << LL_ENDL;
            return false;
        }
        SLRevocation::OmahaUpdate update;
        if (!SLRevocation::parseOmahaReply(reply.body, SLRevocation::CRLSET_APP_ID, update))
        {
            LL_WARNS("Revocation") << "CRLSet update check: unexpected reply" << LL_ENDL;
            return false;
        }
        if (update.status != "ok" || update.codebase.empty())
        {
            // "noupdate": the sequence we sent is still current.
            LL_DEBUGS("Revocation") << "CRLSet " << sSequence << " is current (" << update.status << ")" << LL_ENDL;
            return update.status == "noupdate";
        }
        const long offered = std::strtol(update.version.c_str(), nullptr, 10);
        if (offered > 0 && uint32_t(offered) <= sSequence)
        {
            LL_DEBUGS("Revocation") << "CRLSet " << offered << " offered, " << sSequence << " in force" << LL_ENDL;
            return true;
        }

        std::string crx_url = update.codebase;
        if (crx_url.rfind("http://", 0) == 0)
        {
            crx_url = "https://" + crx_url.substr(7);
        }
        if (crx_url.rfind("https://", 0) != 0)
        {
            LL_WARNS("Revocation") << "CRLSet update: refusing download URL scheme" << LL_ENDL;
            return false;
        }
        GetResult crx = http_get(crx_url, MAX_CRX_BYTES);
        if (!crx.ok)
        {
            LL_WARNS("Revocation") << "CRLSet download failed: HTTP " << crx.code << " " << crx.error << LL_ENDL;
            return false;
        }
        if (LLApp::isExiting()) return false;

        // Off the main thread: hash, CRX3 signature check, unzip, parse.
        std::string crl_bytes, error;
        std::shared_ptr<const SLRevocation::CRLSet> parsed;
        bool hash_ok = true;
        std::string want_sha256 = update.sha256;
        LLStringUtil::toLower(want_sha256);
        run_off_main_thread([&]()
        {
            if (!want_sha256.empty() && sha256_hex(crx.body) != want_sha256)
            {
                hash_ok = false;
                return;
            }
            if (SLRevocation::extractFromCRX3(crx.body, SLRevocation::CRLSET_APP_ID, "crl-set", crl_bytes, &error))
            {
                parsed = SLRevocation::CRLSet::parse(crl_bytes, &error);
            }
        });
        if (!hash_ok)
        {
            LL_WARNS("Revocation") << "CRLSet download does not match the update service's SHA-256" << LL_ENDL;
            return false;
        }
        if (!parsed)
        {
            LL_WARNS("Revocation") << "CRLSet download rejected: " << error << LL_ENDL;
            return false;
        }
        if (parsed->sequence() <= sSequence)
        {
            // Never go backwards (and an equal sequence is the same data).
            LL_INFOS("Revocation") << "CRLSet download has sequence " << parsed->sequence() << ", keeping "
                                   << sSequence << LL_ENDL;
            return true;
        }

        LLFile::mkdir(cache_dir());
        const std::string path = cache_path();
        if (!write_file_atomic(path, crl_bytes))
        {
            LL_WARNS("Revocation") << "Could not store the CRLSet at " << path << LL_ENDL;
            // Still use it for this session.
        }
        else
        {
            sBrowserPath = path;
        }
        LL_INFOS("Revocation") << "Certificate revocation updated (was CRLSet " << sSequence << "): "
                               << describe_crlset(*parsed, "the update service", path) << LL_ENDL;
        sSequence = parsed->sequence();
        SLRevocation::installCRLSet(parsed);
        if (!sBrowserPath.empty())
        {
            hand_to_browser(sBrowserPath);
        }
        return true;
    }

    void fetch_coro()
    {
        struct Running { ~Running() { sFetching = false; } } running;
        const bool ok = fetch_once();
        sNextFetch = LLTimer::getTotalSeconds() + (ok ? REFRESH_INTERVAL : RETRY_INTERVAL);
    }
}

namespace SLRevocationUpdater
{
    void init()
    {
        if (sInitialized) return;
        sInitialized = true;

        // Static distrust list: shipped data. A malformed file is ignored as a whole (and
        // said so), never half-applied.
        size_t distrust_keys = 0, distrust_serials = 0;
        {
            const std::string path = shipped_path("distrusted_spki.txt");
            std::string text, error;
            if (!read_file(path, text, MAX_FILE_BYTES))
            {
                LL_WARNS("Revocation") << "No static distrust list at " << path << LL_ENDL;
            }
            else if (auto list = SLRevocation::DistrustList::parse(text, &error))
            {
                distrust_keys = list->spkiCount();
                distrust_serials = list->serialCount();
                SLRevocation::installDistrustList(list);
            }
            else
            {
                LL_WARNS("Revocation") << "Static distrust list " << path << " ignored: " << error << LL_ENDL;
            }
        }

        // CRLSet: the newer of the cached and the shipped copy.
        std::shared_ptr<const SLRevocation::CRLSet> best;
        std::string best_path, best_source;
        const std::pair<std::string, const char*> candidates[] = {
            { cache_path(), "cache" },
            { shipped_path("crl-set"), "the viewer install" },
        };
        for (const auto& [path, source] : candidates)
        {
            if (!LLFile::isfile(path)) continue;
            std::string error;
            auto set = load_crlset(path, error);
            if (!set)
            {
                LL_WARNS("Revocation") << "CRLSet " << path << " ignored: " << error << LL_ENDL;
                continue;
            }
            if (!best || set->sequence() > best->sequence())
            {
                best = set;
                best_path = path;
                best_source = source;
            }
        }
        std::string crlset_text = "no CRLSet until the first fetch";
        if (best)
        {
            sSequence = best->sequence();
            sBrowserPath = best_path;
            crlset_text = describe_crlset(*best, best_source, best_path);
            SLRevocation::installCRLSet(best);
        }
        LL_INFOS("Revocation") << "Certificate revocation: " << crlset_text << "; static distrust list "
                               << distrust_keys << " keys, " << distrust_serials << " serials; CRLSet updates "
                               << (gSavedSettings.getBOOL("CRLSetUpdateEnabled") ? "on" : "OFF (CRLSetUpdateEnabled)")
                               << LL_ENDL;

        sNextFetch = LLTimer::getTotalSeconds() + FIRST_FETCH_DELAY;
    }

    void idle()
    {
        if (!sInitialized || sFetching || LLApp::isExiting()) return;
        if (LLTimer::getTotalSeconds() < sNextFetch) return;
        if (!gSavedSettings.getBOOL("CRLSetUpdateEnabled"))
        {
            sNextFetch = LLTimer::getTotalSeconds() + REFRESH_INTERVAL;   // look again later
            return;
        }
        sFetching = true;
        LLCoros::instance().launch("SLRevocationUpdater::fetch", []() { fetch_coro(); });
    }

    std::string crlsetPathForBrowser()
    {
        return sBrowserPath;
    }
}
