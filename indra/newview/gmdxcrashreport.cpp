/**
 * @file gmdxcrashreport.cpp
 * @brief GunMetal-DX crash reports (see gmdxcrashreport.h).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
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

// <GMDX> whole file: see gmdxcrashreport.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxcrashreport.h"

#if GMDX_CRASHPAD

#include "llappviewer.h"
#include "lldir.h"
#include "llnotificationsutil.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"

#include <client/crash_report_database.h>
#include <client/crashpad_client.h>
#include <client/settings.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace
{
constexpr const char* kDatabase = "GMDX";   // BugSplat
constexpr const char* kUploadUrl = "https://GMDX.bugsplat.com/post/bp/crash/crashpad.php";
constexpr std::uintmax_t kLogTail = 2u * 1024u * 1024u;   // bytes of the log a report keeps
constexpr size_t kMaxOffered = 5;                        // reports sent at most at once

crashpad::CrashpadClient* sClient = nullptr;
std::unique_ptr<crashpad::CrashReportDatabase> sDatabase;
std::string sDatabaseDir;

base::FilePath to_path(const std::string& utf8)
{
#if LL_WINDOWS
    return base::FilePath(ll_convert_string_to_wide(utf8));
#else
    return base::FilePath(utf8);
#endif
}

// Reports from earlier runs that were never sent (and not asked for yet).
std::vector<crashpad::CrashReportDatabase::Report> unsent()
{
    std::vector<crashpad::CrashReportDatabase::Report> out, list;
    if (!sDatabase) return out;
    for (auto get : { &crashpad::CrashReportDatabase::GetCompletedReports, &crashpad::CrashReportDatabase::GetPendingReports })
    {
        list.clear();
        if (((*sDatabase).*get)(&list) != crashpad::CrashReportDatabase::kNoError) continue;
        for (const auto& r : list)
            if (!r.uploaded && !r.upload_explicitly_requested) out.push_back(r);
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.creation_time > b.creation_time; });
    return out;
}

// The log a report carries, cut to its last kLogTail bytes (the end is what shows the crash).
void trim_attachments(const crashpad::UUID& uuid)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path dir = fs::u8path(sDatabaseDir) / "attachments" / uuid.ToString();
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec))
    {
        const std::uintmax_t size = e.is_regular_file(ec) ? e.file_size(ec) : 0;
        if (ec || size <= kLogTail) continue;
        std::string tail(kLogTail, '\0');
        {
            std::ifstream in(e.path(), std::ios::binary);
            in.seekg((std::streamoff)(size - kLogTail));
            in.read(tail.data(), (std::streamsize)kLogTail);
            tail.resize((size_t)in.gcount());
        }
        std::ofstream out(e.path(), std::ios::binary | std::ios::trunc);
        out << "[... the first " << (size - tail.size()) << " bytes of the log are not included ...]\n" << tail;
    }
}

void send(const std::vector<crashpad::CrashReportDatabase::Report>& reports)
{
    for (size_t i = 0; i < reports.size() && i < kMaxOffered; ++i)
    {
        trim_attachments(reports[i].uuid);
        const auto status = sDatabase->RequestUpload(reports[i].uuid);
        LL_INFOS("GMDXCrash") << "crash report " << reports[i].uuid.ToString() << " sent to BugSplat: "
                              << (status == crashpad::CrashReportDatabase::kNoError ? "queued" : "refused") << LL_ENDL;
    }
    for (size_t i = kMaxOffered; i < reports.size(); ++i) sDatabase->DeleteReport(reports[i].uuid);   // (older ones)
}

void discard(const std::vector<crashpad::CrashReportDatabase::Report>& reports)
{
    for (const auto& r : reports) sDatabase->DeleteReport(r.uuid);
}
} // namespace

void GMDXCrashReport::start()
{
    if (sClient) return;
    sDatabaseDir = gDirUtilp->getOSUserAppDir() + gDirUtilp->getDirDelimiter() + "crashpad";   // <user dir>/crashpad
    {
        std::error_code ec;
        std::filesystem::create_directories(std::filesystem::u8path(sDatabaseDir), ec);
    }
    const std::string exe_dir = gDirUtilp->getExecutableDir();
#if LL_WINDOWS
    const std::string handler = exe_dir + "\\crashpad_handler.exe";
#else
    const std::string handler = exe_dir + "/crashpad_handler";
#endif
    if (!LLFile::isfile(handler))
    {
        LL_WARNS("GMDXCrash") << "no " << handler << ": crash reports are off" << LL_ENDL;
        return;
    }
    const base::FilePath db = to_path(sDatabaseDir);
    sDatabase = crashpad::CrashReportDatabase::Initialize(db);
    if (!sDatabase)
    {
        LL_WARNS("GMDXCrash") << "the crash report database " << sDatabaseDir << " could not be opened" << LL_ENDL;
        return;
    }
    if (crashpad::Settings* settings = sDatabase->GetSettings()) settings->SetUploadsEnabled(false);   // asked first

    const LLVersionInfo& info = LLVersionInfo::instance();
    std::map<std::string, std::string> annotations = {
        { "format", "minidump" },
        { "database", kDatabase },
        { "product", "GunMetal-DX" },
        { "version", info.getVersion() },
        { "key", info.getChannel() },
    };
    std::vector<std::string> arguments = { "--no-rate-limit" };
    std::vector<base::FilePath> attachments = { to_path(LLAppViewer::instance()->getLogFilePath()) };
    sClient = new crashpad::CrashpadClient();
    if (!sClient->StartHandler(to_path(handler), db, db, kUploadUrl, std::string(), annotations, arguments,
                               /*restartable*/ true, /*asynchronous_start*/ false, attachments))
    {
        LL_WARNS("GMDXCrash") << "crashpad_handler did not start: crash reports are off" << LL_ENDL;
        delete sClient;
        sClient = nullptr;
        return;
    }
    LL_INFOS("GMDXCrash") << "crash reports: Crashpad started, database " << sDatabaseDir << LL_ENDL;
}

void GMDXCrashReport::offerPending()
{
    static bool offered = false;
    if (offered || !sClient || !sDatabase) return;
    offered = true;
    const std::vector<crashpad::CrashReportDatabase::Report> reports = unsent();
    if (reports.empty()) return;
    if (gSavedSettings.getS32("GMDXCrashReportSend") == 1)
    {
        send(reports);
        return;
    }
    LLSD args;
    args["COUNT"] = (S32)reports.size();
    LLNotificationsUtil::add(reports.size() == 1 ? "GMDXCrashReportOffer" : "GMDXCrashReportOfferMany", args, LLSD(),
        [reports](const LLSD& n, const LLSD& r)
        {
            switch (LLNotificationsUtil::getSelectedOption(n, r))
            {
                case 1: gSavedSettings.setS32("GMDXCrashReportSend", 1); [[fallthrough]];
                case 0: send(reports); break;
                case 2: discard(reports); break;
                default: break;   // closed: asked again next time
            }
        });
}

#else   // no Crashpad package in this build

void GMDXCrashReport::start() {}
void GMDXCrashReport::offerPending() {}

#endif
