/**
 * @file llcleanalert.h
 * @brief #29 automated alert sink. When a clean-pipeline detector confirms a
 *        never-healing anomaly (persistent strand, geometry corruption), it
 *        calls CleanAlert::fire() with a reason tag and a forensic body. fire()
 *        (a) emits a loud `#29ALERT` log line (grep token for humans), and
 *        (b) writes a SELF-CONTAINED forensic file
 *            <logs>/clean29_alert_<frame>_<reason>.log
 *        whose creation is the machine-watchable hook: an external harness
 *        (or Claude watching the log dir) is alerted the moment one appears,
 *        with everything needed to start diagnosis inside it.
 *
 *        Throttled per reason so a sustained anomaly can't spam the disk.
 *        Header-only; CLEAN_DEBUG-gated at the call sites, not here.
 */

#pragma once

#include "lldir.h"
#include "llappviewer.h"   // gFrameCount

#include <cstdio>
#include <map>
#include <string>

namespace CleanAlert
{

// Write one alert. `reason` is a short slug (e.g. "strand", "corruption");
// `body` is the pre-formatted forensic text. Returns the file path written,
// or empty when throttled / on failure.
inline std::string fire(const std::string& reason, const std::string& body)
{
    // Per-reason throttle: at most one alert file per ~60s (3600 frames @60).
    static std::map<std::string, U32> s_last_fire;
    U32& last = s_last_fire[reason];
    if (last != 0u && (gFrameCount - last) < 3600u) return std::string();
    last = gFrameCount;

    const std::string path = gDirUtilp->getExpandedFilename(
        LL_PATH_LOGS,
        "clean29_alert_" + std::to_string((unsigned)gFrameCount) + "_" + reason + ".log");

    LL_WARNS("Clean29") << "#29ALERT reason=" << reason
        << " frame=" << gFrameCount << " -> " << path << LL_ENDL;

    if (FILE* f = fopen(path.c_str(), "w"))
    {
        fprintf(f, "#29ALERT reason=%s frame=%u\n\n%s\n",
                reason.c_str(), (unsigned)gFrameCount, body.c_str());
        fclose(f);
        return path;
    }
    return std::string();
}

} // namespace CleanAlert
