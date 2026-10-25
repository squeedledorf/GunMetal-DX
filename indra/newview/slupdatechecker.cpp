/**
 * @file slupdatechecker.cpp
 * @brief Update check (#293, phase 1). See slupdatechecker.h.
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

#include "slupdatechecker.h"
#include "slupdateapply.h"
#include "slupdater.h"

#include <array>
#include <cstdlib>
#include <map>
#include <string>

#include <boost/json.hpp>

#include "llapp.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldir.h"
#include "llfile.h"
#include "llnotificationsutil.h"
#include "llstartup.h"
#include "lltimer.h"
#include "lltrans.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llweb.h"

namespace
{
    // The feed is a few hundred bytes; anything near this is not our feed.
    constexpr size_t MAX_FEED_BYTES = 64 * 1024;
    constexpr unsigned int TIMEOUT_SECONDS = 20;
    // Every version's notes; opened after an update when the feed can't name this build's.
    constexpr const char* RELEASE_NOTES_FALLBACK = "https://slipstreamviewer.org/releases.html";

    using Version = std::array<U64, 4>;

    struct Feed
    {
        Version     version{};
        std::string build;      // as the feed spells it, e.g. "1.1.0.1"
        std::string label;      // e.g. "1.1.0 Beta 1"
        std::string notes_url;
        std::map<std::string, std::string> downloads;   // windows_setup, linux_tarball, ...
        // Signed update manifest (#421), passed on verbatim to SLUpdater, which verifies the
        // signature before reading anything in it. Empty when the feed has none.
        std::string update_manifest;
        std::string update_signature;
    };

    // How this copy was installed; decides what "Download" opens, or whether the
    // package manager does the updating.
    enum class Install { WindowsSetup, WindowsZip, LinuxTarball, Flatpak, Deb, Aur, Other };

    double sNextCheck = 0.0;    // LLTimer::getTotalSeconds() of the next scheduled check; 0 = not scheduled
    bool sInFlight = false;
    std::string sNotifiedBuild; // already offered this session (scheduled checks don't repeat it)
    bool sWarnedBadFeed = false;

    // "1.2.3.4" -> {1,2,3,4}; digits and exactly four parts only.
    bool parse_version(const std::string& text, Version& out)
    {
        size_t part = 0, digits = 0;
        U64 value = 0;
        for (char c : text)
        {
            if (c >= '0' && c <= '9')
            {
                if (++digits > 9) return false;
                value = value * 10 + (U64)(c - '0');
            }
            else if (c == '.' && digits > 0 && part < 3)
            {
                out[part++] = value;
                value = 0;
                digits = 0;
            }
            else
            {
                return false;
            }
        }
        if (part != 3 || digits == 0) return false;
        out[3] = value;
        return true;
    }

    // The running build, or UpdateCheckFakeVersion when set (testing).
    Version running_version(std::string& display)
    {
        Version v{};
        const std::string fake = gSavedSettings.getString("UpdateCheckFakeVersion");
        if (!fake.empty() && parse_version(fake, v))
        {
            display = fake;
            return v;
        }
        const LLVersionInfo& info = LLVersionInfo::instance();
        v = { (U64)info.getMajor(), (U64)info.getMinor(), (U64)info.getPatch(), info.getBuild() };
        display = llformat("%llu.%llu.%llu.%llu", (unsigned long long)v[0], (unsigned long long)v[1],
                           (unsigned long long)v[2], (unsigned long long)v[3]);
        return v;
    }

    // Test/local builds (channel "Slipstream Test", BUILD 0) don't check on their own.
    bool is_test_build()
    {
        const LLVersionInfo& info = LLVersionInfo::instance();
        const LLVersionInfo::ViewerMaturity maturity = info.getViewerMaturity();
        return info.getBuild() == 0
            || (maturity != LLVersionInfo::RELEASE_VIEWER && maturity != LLVersionInfo::BETA_VIEWER);
    }

    // Release builds read the release feed unless the person opted into betas
    // (UpdaterWillingToTest, Preferences > Setup "Also offer beta versions");
    // Jetstream builds read the Jetstream feed (newest build from main); everything else
    // reads the beta feed, which also carries finals.
    std::string feed_url()
    {
        if (SLUpdateApply::isJetstreamBuild())
        {
            return gSavedSettings.getString("UpdateCheckURLJetstream");
        }
        const bool release = LLVersionInfo::instance().getViewerMaturity() == LLVersionInfo::RELEASE_VIEWER
                             && !gSavedSettings.getBOOL("UpdaterWillingToTest");
        return gSavedSettings.getString(release ? "UpdateCheckURLRelease" : "UpdateCheckURLBeta");
    }

    bool is_release_feed()
    {
        return LLVersionInfo::instance().getViewerMaturity() == LLVersionInfo::RELEASE_VIEWER
               && !gSavedSettings.getBOOL("UpdaterWillingToTest");
    }

    bool is_https(const std::string& url)
    {
        return url.compare(0, 8, "https://") == 0 && url.size() > 8;
    }

#if LL_LINUX
    std::string read_marker(const std::string& path)
    {
        if (!LLFile::isfile(path)) return std::string();
        llifstream in(path.c_str());
        std::string line;
        std::getline(in, line);
        LLStringUtil::trim(line);
        return line;
    }
#endif

    Install detect_install()
    {
#if LL_WINDOWS
        // The installer records what it installed in uninstall_list.nsh.txt next to the
        // program; an unpacked .zip has no such file.
        const std::string list = gDirUtilp->getExecutableDir() + gDirUtilp->getDirDelimiter() + "uninstall_list.nsh.txt";
        return LLFile::isfile(list) ? Install::WindowsSetup : Install::WindowsZip;
#elif LL_LINUX
        if (std::getenv("FLATPAK_ID") || LLFile::isfile("/.flatpak-info"))
        {
            return Install::Flatpak;
        }
        // Distribution packages drop a .packaged-by marker in the install root
        // (scripts/packaging/*): "deb", "flatpak", or the AUR package name. The launcher
        // runs us from that root; check next to bin/ too in case it didn't.
        std::string by = read_marker(gDirUtilp->getAppRODataDir() + "/.packaged-by");
        if (by.empty())
        {
            by = read_marker(gDirUtilp->getExecutableDir() + "/../.packaged-by");
        }
        if (by.empty()) return Install::LinuxTarball;
        if (by == "flatpak") return Install::Flatpak;
        if (by == "deb") return Install::Deb;
        return Install::Aur;
#else
        return Install::Other;
#endif
    }

    Install install_kind()
    {
        static const Install kind = detect_install();
        return kind;
    }

    std::string download_url(const Feed& feed)
    {
        const char* first = nullptr;
        const char* second = nullptr;
        switch (install_kind())
        {
            case Install::WindowsSetup: first = "windows_setup"; second = "windows_zip"; break;
            case Install::WindowsZip:   first = "windows_zip";   second = "windows_setup"; break;
            case Install::LinuxTarball: first = "linux_tarball"; break;
            default: break;
        }
        for (const char* key : { first, second })
        {
            if (!key) continue;
            auto it = feed.downloads.find(key);
            if (it != feed.downloads.end()) return it->second;
        }
        return feed.notes_url;   // the release notes link to every download
    }

    // The feed text is shown in a notification; keep the label to plain version text.
    std::string clean_label(const std::string& label, const std::string& fallback)
    {
        std::string out;
        for (char c : label)
        {
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')
                || c == ' ' || c == '.' || c == '-' || c == '(' || c == ')')
            {
                out += c;
            }
        }
        LLStringUtil::trim(out);
        return out.empty() || out.size() > 64 ? fallback : out;
    }

    std::string json_string(const boost::json::object& obj, const char* key)
    {
        const boost::json::value* v = obj.if_contains(key);
        return v && v->is_string() ? std::string(v->as_string()) : std::string();
    }

    // Untrusted input: anything missing or odd makes the whole feed invalid.
    bool parse_feed(const std::string& text, Feed& feed)
    {
        boost::system::error_code ec;
        const boost::json::value doc = boost::json::parse(text, ec);
        if (ec || !doc.is_object()) return false;
        const boost::json::object& obj = doc.as_object();
        feed.build = json_string(obj, "build");
        if (!parse_version(feed.build, feed.version)) return false;
        feed.label = clean_label(json_string(obj, "label"), feed.build);
        feed.notes_url = json_string(obj, "notes_url");
        if (!is_https(feed.notes_url)) return false;
        if (const boost::json::value* dl = obj.if_contains("downloads"); dl && dl->is_object())
        {
            for (const auto& entry : dl->as_object())
            {
                if (entry.value().is_string() && is_https(std::string(entry.value().as_string())))
                {
                    feed.downloads[std::string(entry.key())] = std::string(entry.value().as_string());
                }
            }
        }
        if (const boost::json::value* up = obj.if_contains("update"); up && up->is_object())
        {
            feed.update_manifest = json_string(up->as_object(), "manifest");
            feed.update_signature = json_string(up->as_object(), "signature");
            if (feed.update_manifest.size() > 32 * 1024 || feed.update_signature.size() > 512)
            {
                feed.update_manifest.clear();
                feed.update_signature.clear();
            }
        }
        return true;
    }

    void warn_bad_feed(const std::string& why)
    {
        if (!sWarnedBadFeed)
        {
            sWarnedBadFeed = true;
            LL_WARNS("UpdateCheck") << "Update check: " << why << LL_ENDL;
        }
    }

    void notify_failed(bool manual)
    {
        if (manual)
        {
            LLNotificationsUtil::add("UpdateCheckFailed");
        }
    }

    // A manual check answers with a dialog: toasts aren't shown until after login, and
    // the person is waiting for an answer anyway. The daily check uses a toast.
    void offer_update(const Feed& feed, const std::string& current, bool manual)
    {
        const std::string suffix = manual ? "Modal" : "";
        sNotifiedBuild = feed.build;
        LLSD args;
        args["VERSION"] = feed.label;
        args["CURRENT"] = current;
        const std::string notes = feed.notes_url;
        const Install kind = install_kind();
        if (kind == Install::Flatpak || kind == Install::Deb || kind == Install::Aur)
        {
            LLStringUtil::format_map_t how_args;
            const char* app_id = std::getenv("FLATPAK_ID");
            how_args["[APP_ID]"] = app_id && *app_id ? std::string(app_id) : std::string("org.slipstreamviewer.Slipstream");
            args["HOW"] = LLTrans::getString(kind == Install::Flatpak ? "UpdateHowFlatpak"
                                             : kind == Install::Deb   ? "UpdateHowDeb"
                                                                      : "UpdateHowAUR", how_args);
            LLNotificationsUtil::add("UpdateAvailablePackaged" + suffix, args, LLSD(),
                [notes](const LLSD& notification, const LLSD& response)
                {
                    if (LLNotificationsUtil::getSelectedOption(notification, response) == 0)
                    {
                        LLWeb::loadURLExternal(notes);
                    }
                });
            return;
        }
        const std::string download = download_url(feed);
        const std::string build = feed.build;
        LLNotificationsUtil::add("UpdateAvailable" + suffix, args, LLSD(),
            [notes, download, build](const LLSD& notification, const LLSD& response)
            {
                switch (LLNotificationsUtil::getSelectedOption(notification, response))
                {
                    case 0: LLWeb::loadURLExternal(notes); break;
                    case 1: LLWeb::loadURLExternal(download); break;
                    case 2: gSavedSettings.setString("UpdateCheckSkippedBuild", build); break;
                    default: break;
                }
            });
    }

    enum class FetchResult { Ok, HttpFailed, Malformed, Exiting };

    // GET and parse the feed (suspends the calling coroutine).
    FetchResult fetch_feed(const std::string& url, Feed& feed, std::string& why)
    {
        LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter(new LLCoreHttpUtil::HttpCoroutineAdapter("UpdateCheck", policy));
        LLCore::HttpRequest::ptr_t request(new LLCore::HttpRequest);
        LLCore::HttpOptions::ptr_t options(new LLCore::HttpOptions);
        options->setTimeout(TIMEOUT_SECONDS);
        options->setRetries(0);
        options->setFollowRedirects(false);
        options->setMaxBodyBytes(MAX_FEED_BYTES);
        LLCore::HttpHeaders::ptr_t headers(new LLCore::HttpHeaders);
        headers->append(HTTP_OUT_HEADER_ACCEPT, HTTP_CONTENT_JSON);

        LLSD result = adapter->getRawAndSuspend(request, url, options, headers);
        if (LLApp::isExiting()) return FetchResult::Exiting;

        const LLSD& http = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http);
        if (!status)
        {
            why = status.toString();
            return FetchResult::HttpFailed;
        }
        const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        if (raw.size() > MAX_FEED_BYTES || !parse_feed(std::string(raw.begin(), raw.end()), feed))
        {
            why = "malformed feed from " + url;
            return FetchResult::Malformed;
        }
        return FetchResult::Ok;
    }

    void check_coro(std::string url, bool manual)
    {
        Feed feed;
        std::string why;
        const FetchResult fetched = fetch_feed(url, feed, why);
        sInFlight = false;
        if (fetched == FetchResult::Exiting) return;
        if (fetched == FetchResult::HttpFailed)
        {
            // Offline, site down, proxy trouble: try again at the next interval.
            LL_INFOS("UpdateCheck") << "Update check failed: " << why << LL_ENDL;
            notify_failed(manual);
            return;
        }
        if (fetched == FetchResult::Malformed)
        {
            warn_bad_feed("ignoring " + why);
            notify_failed(manual);
            return;
        }

        std::string current;
        const Version running = running_version(current);
        const bool newer = feed.version > running;
        LL_INFOS("UpdateCheck") << "Update check: running " << current << ", feed offers " << feed.build
                                << (newer ? " (newer)" : "") << LL_ENDL;
        if (!newer)
        {
            if (manual)
            {
                LLSD args;
                args["CURRENT"] = current;
                LLNotificationsUtil::add("UpdateCheckUpToDate", args);
            }
            return;
        }
        // Auto-install (#421, setting UpdateAutoInstall, default off): a signed manifest that
        // verifies means the installer is downloaded in the background and applied at the next
        // start; SLUpdater then announces it itself. Anything it can't do falls through to the
        // normal notice below.
        if (SLUpdater::onFeedUpdate(feed.update_manifest, feed.update_signature, feed.build, current,
                                    is_release_feed()))
        {
            sNotifiedBuild = feed.build;
            return;
        }
        // A manual check shows the offer even for a skipped version (the way to un-skip).
        if (!manual && (feed.build == gSavedSettings.getString("UpdateCheckSkippedBuild")
                        || feed.build == sNotifiedBuild))
        {
            return;
        }
        offer_update(feed, current, manual);
    }

    void start_check(bool manual)
    {
        const std::string url = feed_url();
        if (!is_https(url))
        {
            // Blank (labs / enterprise switched it off) or not HTTPS: never fetch.
            if (!url.empty()) warn_bad_feed("update feed URL is not https, not checking: " + url);
            notify_failed(manual);
            return;
        }
        if (sInFlight)
        {
            return;
        }
        sInFlight = true;
        LLCoros::instance().launch("SLUpdateChecker::check", [url, manual]() { check_coro(url, manual); });
    }

    // First launch on a new version: the feed's notes_url when the feed describes the
    // build that is running, else the release notes page (all versions).
    void release_notes_coro(std::string url)
    {
        std::string notes = RELEASE_NOTES_FALLBACK;
        if (!is_https(url))
        {
            LL_WARNS("UpdateCheck") << "Release notes: update feed URL is not https (\"" << url
                                    << "\"), opening " << notes << LL_ENDL;
        }
        else
        {
            Feed feed;
            std::string why;
            const FetchResult fetched = fetch_feed(url, feed, why);
            if (fetched == FetchResult::Exiting) return;
            std::string current;
            const Version running = running_version(current);
            if (fetched != FetchResult::Ok)
            {
                LL_WARNS("UpdateCheck") << "Release notes: could not read " << url << " (" << why
                                        << "), opening " << notes << LL_ENDL;
            }
            else if (feed.version != running)
            {
                LL_INFOS("UpdateCheck") << "Release notes: feed describes " << feed.build << ", running "
                                        << current << ", opening " << notes << LL_ENDL;
            }
            else
            {
                notes = feed.notes_url;   // parse_feed guarantees https
            }
        }
        LL_INFOS("UpdateCheck") << "Release notes: opening " << notes << LL_ENDL;
        LLWeb::loadURLInternal(notes);
    }
}

namespace SLUpdateChecker
{
    void idle()
    {
        if (LLStartUp::getStartupState() < STATE_STARTED || is_test_build()) return;
        // <GMDX> Nothing is fetched until the person says so, once (GMDXUpdateConsent -1): check and
        // install automatically, check and tell, or never check (Help > Check for updates still works).
        static LLCachedControl<S32> consent(gSavedSettings, "GMDXUpdateConsent", -1);
        if (consent < 0)
        {
            static bool asked = false;
            if (!asked)
            {
                asked = true;
                LLNotificationsUtil::add("GMDXUpdateConsent", LLSD(), LLSD(), [](const LLSD& n, const LLSD& r)
                    {
                        const S32 option = LLNotificationsUtil::getSelectedOption(n, r);
                        if (option < 0 || option > 2) return;   // closed: ask again next session
                        gSavedSettings.setBOOL("UpdateCheckEnabled", option != 2);
                        if (option != 2) gSavedSettings.setBOOL("UpdateAutoInstall", option == 0);
                        gSavedSettings.setS32("GMDXUpdateConsent", option == 2 ? 0 : 1);
                    });
            }
            return;
        }
        // </GMDX>
        static LLCachedControl<bool> enabled(gSavedSettings, "UpdateCheckEnabled", true);
        if (!enabled) return;
        const double now = LLTimer::getTotalSeconds();
        if (sNextCheck == 0.0)
        {
            sNextCheck = now + FIRST_CHECK_DELAY;
            return;
        }
        if (now < sNextCheck || sInFlight) return;
        sNextCheck = now + CHECK_INTERVAL;
        start_check(false);
    }

    void checkNow()
    {
        start_check(true);
    }

    void showReleaseNotes()
    {
        const std::string url = feed_url();
        LLCoros::instance().launch("SLUpdateChecker::releaseNotes", [url]() { release_notes_coro(url); });
    }

    const char* installArtifactKey()
    {
        switch (install_kind())
        {
            case Install::WindowsSetup: return "windows_setup";
            case Install::LinuxTarball: return "linux_tarball";
            default: return nullptr;
        }
    }

    bool isReleaseBuild()
    {
        return LLVersionInfo::instance().getViewerMaturity() == LLVersionInfo::RELEASE_VIEWER;
    }
}
