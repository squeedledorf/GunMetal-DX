/**
 * @file llfloaterreportbug.cpp
 * @brief Help > Report Bug: account-free bug reports through the Slipstream report relay.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Bluefast Studios
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
#include "llgpuinfo.h"

#include "llfloaterreportbug.h"

#include "llagent.h"
#include "llappviewer.h"
#include "llavatarname.h"
#include "llavatarnamecache.h"
#include "llbase64.h"
#include "llbutton.h"
#include "llcallingcard.h"
#include "llcheckboxctrl.h"
#include "llcorehttputil.h"
#include "lldiriterator.h"
#include "llfile.h"
#include "llcoros.h"
#include "llerrorcontrol.h"
#include "llfloaterreg.h"
#include "llgl.h"
#include "llimagejpeg.h"
#include "lllineeditor.h"
#include "llnotificationsutil.h"
#include "llreporttracker.h"
#include "llrender2dutils.h"
#include "llscrolllistctrl.h"
#include "llsdjson.h"
#include "llstartup.h"
#include "lltabcontainer.h"
#include "lltextbox.h"
#include "lltexteditor.h"
#include "lltrans.h"
#include "llversioninfo.h"
#include "llviewercontrol.h"
#include "llviewernetwork.h"
#include "llviewertexture.h"
#include "llviewerwindow.h"
#include "llmemdiag.h"
#include "llmemorygovernor.h"
#include "lltimer.h"
#include "llvkperfcapture.h"   // #966 performance capture attachment
#include "vkgfx/llvulkancontext.h"   // VRAM budget / accounting line (#405 / #407)
#include "llweb.h"
#include "llworld.h"
#include "sldatausage.h"

#include <boost/json.hpp>
#include <boost/regex.hpp>
#include <vulkan/vulkan.h>

#include <algorithm>
#include <deque>
#include <fstream>
#include <map>

// The relay signature key (SLIPSTREAM_REPORT_HMAC_KEY) lives in llreporttracker.h.

namespace
{
    // Relay limits (relay/slipstream_report_relay.py); checked here so the user gets a
    // clear message instead of a server rejection.
    constexpr S32 SUMMARY_MIN = 8, SUMMARY_MAX = 120;
    constexpr S32 DESCRIPTION_MIN = 10, DESCRIPTION_MAX = 8000;
    constexpr S32 STEPS_MAX = 4000;
    constexpr S32 FOLLOWUP_MIN = 5, FOLLOWUP_MAX = 4000;
    constexpr S32 AVATAR_MAX = 64;
    constexpr size_t SYSTEM_VALUE_MAX = 200;
    constexpr size_t LOG_MAX_BYTES = 192 * 1024;
    constexpr S32 LOG_MAX_LINES = 1500;
    constexpr size_t LOG_READ_TAIL = 4 * 1024 * 1024;   // only the end of the log is read
    // #790: an earlier session that crashed / lost the graphics device: the tail of its log
    // and the device-loss report, each under its own budget; the current / main log gets
    // what is left of LOG_MAX_BYTES (the relay takes 256 KB / 4000 lines in all).
    constexpr size_t PREV_LOG_MAX_BYTES = 56 * 1024;
    constexpr S32 PREV_LOG_MAX_LINES = 600;
    constexpr size_t PREV_LOG_READ_TAIL = 1024 * 1024;
    constexpr size_t GPU_LOST_MAX_BYTES = 16 * 1024;
    constexpr S32 GPU_LOST_MAX_LINES = 200;
    constexpr S32 SHOT_MAX_WIDTH = 1600;
    constexpr size_t SHOT_MAX_BYTES = 3 * 1024 * 1024;

    // Settings that describe the render setup (Help > About plus the graphics prefs).
    const char* const REPORT_SETTINGS[] = {
        "RenderQualityPerformance", "RenderFarClip", "RenderVKUpscaler", "RenderVKDlssRayReconstruction", "RenderVKRenderScale",
        "RenderVKOpaqueShadingRate", "RenderVKAlphaShadingRate", "RenderShadowDetail",
        "RenderVKSunShadows", "RenderVKShadowResolution", "RenderVKProjectorShadows",
        "RenderReflectionProbeDetail", "RenderReflectionProbeLevel", "RenderScreenSpaceReflections",
        "RenderMirrors", "RenderVKTAA", "RenderVKGodRays", "RenderVKRayTracing", "RenderDeferredSSAO",
        "RenderVKAOStandard", "RenderFSAAType", "RenderTransparentWater", "RenderDepthOfField",
        "SlipstreamRenderTonemapType", "RenderVSyncEnable", "RenderVKFramePacing", "RenderVKNoVsyncPresentMode",
        "RenderFPSLimit", "RenderMaxTextureResolution",
        "RenderVolumeLODFactor", "RenderHiDPI", "UIScaleFactor", "RenderMaxVideoMemoryMB",
        "RenderVKTextureCompression", "RenderReflectionProbeResolution", "RenderReflectionProbeCount",
        "RenderVKPipelineCompileThreads", "RenderVKPipelineCompileBudgetMB", "VKDebugForceIntelCompileCap",
    };

    // Replace every byte sequence that is not valid UTF-8 with U+FFFD. The log excerpt is
    // read as raw bytes (Windows code-page paths, garbled names) and the JSON writer passes
    // them through; the relay's strict decoder then rejected the whole report as malformed.
    std::string valid_utf8(const std::string& s)
    {
        std::string out;
        out.reserve(s.size());
        const size_t n = s.size();
        size_t i = 0;
        while (i < n)
        {
            const U8 c = static_cast<U8>(s[i]);
            size_t len = 0;
            U32 lo = 0x80, hi = 0xBF;   // allowed range of the 2nd byte (overlongs / surrogates / > U+10FFFF out)
            if (c < 0x80)                    len = 1;
            else if (c >= 0xC2 && c <= 0xDF) len = 2;
            else if (c >= 0xE0 && c <= 0xEF) { len = 3; if (c == 0xE0) lo = 0xA0; if (c == 0xED) hi = 0x9F; }
            else if (c >= 0xF0 && c <= 0xF4) { len = 4; if (c == 0xF0) lo = 0x90; if (c == 0xF4) hi = 0x8F; }
            bool ok = len > 0 && i + len <= n;
            for (size_t k = 1; ok && k < len; ++k)
            {
                const U8 b = static_cast<U8>(s[i + k]);
                ok = (k == 1) ? (b >= lo && b <= hi) : ((b & 0xC0) == 0x80);
            }
            if (ok) { out.append(s, i, len); i += len; }
            else    { out.append("\xEF\xBF\xBD"); ++i; }
        }
        return out;
    }

    std::string clip(const std::string& s, size_t n)
    {
        if (s.size() <= n) return s;
        std::string out = s.substr(0, n);
        while (!out.empty() && (static_cast<U8>(out.back()) & 0xC0) == 0x80) out.pop_back(); // no split UTF-8
        if (!out.empty() && (static_cast<U8>(out.back()) & 0x80)) out.pop_back();
        return out;
    }

    // Driver name/version: Help > About only has it on Windows (WMI). Ask Vulkan with a
    // throwaway instance (VkPhysicalDeviceDriverProperties, core 1.2); cached once.
    std::string query_vulkan_driver(const std::string& device_name)
    {
        static std::string sCached;
        static bool sQueried = false;
        if (sQueried) return sCached;
        sQueried = true;

        VkApplicationInfo app{ VK_STRUCTURE_TYPE_APPLICATION_INFO };
        app.pApplicationName = "Slipstream report";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ci{ VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        ci.pApplicationInfo = &app;
        VkInstance inst = VK_NULL_HANDLE;
        if (vkCreateInstance(&ci, nullptr, &inst) != VK_SUCCESS || !inst) return sCached;

        uint32_t count = 0;
        vkEnumeratePhysicalDevices(inst, &count, nullptr);
        std::vector<VkPhysicalDevice> devs(count);
        if (count) vkEnumeratePhysicalDevices(inst, &count, devs.data());
        std::string first;
        for (VkPhysicalDevice dev : devs)
        {
            VkPhysicalDeviceProperties base{};
            vkGetPhysicalDeviceProperties(dev, &base);
            std::string version;
            if (base.apiVersion >= VK_API_VERSION_1_2)
            {
                VkPhysicalDeviceDriverProperties drv{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
                VkPhysicalDeviceProperties2 p2{ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
                p2.pNext = &drv;
                vkGetPhysicalDeviceProperties2(dev, &p2);
                version = std::string(drv.driverName) + " " + drv.driverInfo;
            }
            else
            {
                version = llformat("driver version 0x%08x", base.driverVersion);
            }
            LLStringUtil::trim(version);
            if (first.empty()) first = version;
            if (device_name == base.deviceName)
            {
                sCached = version;
                break;
            }
        }
        if (sCached.empty()) sCached = first;
        vkDestroyInstance(inst, nullptr);
        return sCached;
    }

    std::string exec_event_name(eLastExecEvent ev)
    {
        // A lost graphics device closes the viewer cleanly (no crash marker, #790), so it is
        // its own flag: report it too, it is the most common "crash" on high settings.
        if (ev == LAST_EXEC_NORMAL && gLastExecDeviceLost)
            return "lost the graphics device (the graphics driver stopped responding)";
        switch (ev)
        {
            case LAST_EXEC_FROZE:          return "froze (closed while not responding)";
            case LAST_EXEC_LLERROR_CRASH:  return "crashed (internal error)";
            case LAST_EXEC_OTHER_CRASH:    return "crashed";
            case LAST_EXEC_LOGOUT_CRASH:   return "crashed while logging out";
            case LAST_EXEC_BAD_ALLOC:      return "ran out of memory";
            default:                       return "";
        }
    }

    // ------------------------------------------------------------------ log excerpt

    // Keep every line (all levels, all sources) and scrub personal information out of it:
    // names, chat / IM text, credentials and handles, addresses and the secret parts of
    // URLs. Hardware, graphics and audio device details are not personal and stay.
    // "<time> <LEVEL> #<tag>#<tag># <file>(<line>) <function> : <message>"; just "#" with no tags.
    const boost::regex LOG_HEADER("^\\d{4}-\\d\\d-\\d\\dT\\d\\d:\\d\\d:\\d\\dZ (DEBUG|INFO|WARNING|ERROR) #((?:[^#\\s]+#)*) ([^ (]+)(\\(\\d+\\))?");
    // Tags whose lines carry long single-line timing / renderer reports (#VKGpu# sub-pass
    // marks, #VKHitch# SECTIONS, ...): clipped at PERF_LINE_MAX instead of 600.
    const boost::regex PERF_TAGS("(^|#)(VKGpu|VKHitch|VKPace|Vulkan)#");
    constexpr size_t PERF_LINE_MAX = 8192;
    // Sources whose message body is user-written text (chat, IMs, conversation logs): the
    // header stays, the body is replaced.
    const boost::regex CHAT_SOURCES("(llimview|llimprocessing|llnearbychat|llfloaterim|llchat|alchat|lllogchat|llconversation"
                                    "|llfloaterimsession|llfloaterimnearbychat|llnearbychathandler|llgroupmgr\\.cpp.*notice)",
                                    boost::regex::icase);
    const boost::regex CHAT_TAGS("(^|#)(Chat|IM|InstantMessage|Conversation)#", boost::regex::icase);
    // Lines that carry auth material: every UUID on them is masked, not just the keyed one.
    const boost::regex AUTH_WORDS("(session_id|sessionid|secure_session|agent_session|session handle|session_handle"
                                  "|connector_handle|account_handle|auth_token|access_token|cookie|authorization|password|passwd)",
                                  boost::regex::icase);
    const boost::regex SCRUB_UUID("\\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\\b");
    // Whole-value secrets: the value runs to the end of the line (cookie and header values
    // contain spaces and separators).
    const boost::regex SCRUB_SECRET_EOL("\\b(authorization|set-cookie|cookie|password|passwd|from_name|fromname"
                                        "|display name|legacy name|user_name|username)(\"?'?\\s*[=:]\\s*)[^\\r\\n]*",
                                        boost::regex::icase);
    // Voice account / connector / session handles and channel credentials.
    const boost::regex SCRUB_VOICE_KV("\\b(account_?(?:name|handle|uri)?|connector_?handle|session_?(?:group_?)?handle"
                                      "|channel_?uri|channel_?credentials|credentials|jwt|voice_?token|ice-ufrag|ice-pwd"
                                      "|ufrag|pwd|fingerprint|sdp)(\"?'?\\s*[=:]\\s*)[^\\s,;&\"'<>]+",
                                      boost::regex::icase);
    const boost::regex SCRUB_SIP("\\bsips?:[^\\s\"'<>]+", boost::regex::icase);
    // SDP / ICE candidate lines carry IP addresses and ICE credentials.
    const boost::regex SDP_LINE("(^|\\s)(a=(?:candidate|ice-|fingerprint|rtcp|setup|mid|group)|c=IN |o=[-\\w]+ \\d+ |m=(?:audio|video|application)|candidate:)",
                                boost::regex::icase);
    // Elevated priority when the byte budget forces a choice: voice / audio / hardware.
    const boost::regex DEVICE_LINES("(voice|webrtc|audio|fmod|playout|recording|microphone|speaker|sound|device|adapter"
                                    "|\\bgpu\\b|\\bcpu\\b|driver|vulkan|\\bvk\\b|hardware|memory|\\bram\\b|vram)",
                                    boost::regex::icase);
    const boost::regex VOICE_LINES("(voice|webrtc|playout|recording|microphone)", boost::regex::icase);
    // The periodic memory / VRAM status reports (#739): the newest of each kind is always kept.
    const boost::regex MEM_STATUS_TAGS("(^|#)(VKMem|MemGov)#");
    // High-volume periodic INFO that would crowd out the rest.
    const boost::regex NOISY_LINES("(AssetFetch|asset fetch|timeline|\\btick\\b|\\bstats?\\b)", boost::regex::icase);
    const boost::regex SCRUB_USERINFO("\\b([a-zA-Z][a-zA-Z0-9+.-]*://)[^\\s/@]+@");
    const boost::regex SCRUB_QUERY("([?&](?:key|auth|session|cap|token|sig|code|id)=)[^&\\s\"'<>]+", boost::regex::icase);
    const boost::regex SCRUB_KV("\\b(token|access_token|apikey|api_key|secret|signature|session_id|secure_session_id|sessionid)(\\s*[=:]\\s*)[^\\s&\"'<>]+",
                                boost::regex::icase);
    const boost::regex SCRUB_CAP("(/cap/)[0-9a-fA-F-]{8,}");
    // Capability URLs hide their secret in the path: keep scheme + host (+ port), mask the rest.
    const boost::regex SCRUB_URL_PATH("\\b([a-zA-Z][a-zA-Z0-9+.-]*://[^\\s/?#\"'<>]+)([/?#][^\\s\"'<>]*)");
    const boost::regex SCRUB_IPV4("\\b((?:\\d{1,3}\\.){3}\\d{1,3})(?::\\d{1,5})?\\b");
    // A dotted quad right after a version cue is a version, not an address:
    // "Version: Slipstream Beta 1.1.0.9", "build 1.2.3.4", "SecondLife/1.1.0.9".
    const boost::regex VERSION_CUE("(?:\\b(?:version|ver|build)\\b"
                                   "|\\b(?:slipstream|alchemy|second ?life)(?:[ _-]?(?:beta|release|alpha|test|develop|nightly|project|os)\\b)*)"
                                   "[\\s:=/#(]*$",
                                   boost::regex::icase);
    const boost::regex SCRUB_IPV6("(?<![\\w:])(?=[0-9a-fA-F:]*\\d)(?:[0-9a-fA-F]{1,4}:){3,7}[0-9a-fA-F]{1,4}(?![\\w:])"
                                  "|(?<![\\w:])(?=[0-9a-fA-F:]*\\d)(?:[0-9a-fA-F]{1,4}:){1,6}:(?:[0-9a-fA-F]{1,4}:){0,5}[0-9a-fA-F]{1,4}(?![\\w:])");
    // Machine identifiers (#790): unique per computer, used for bans and fingerprinting --
    // personal data. LL's LLWMIMethods / LLMachineID logged them at INFO ("Serial Number :
    // <Windows product id / SMBIOS UUID>", "UniqueID: 0x<hex>"), the login request carries
    // 'mac' / 'id0' (DEBUG dump), and MAC addresses can turn up anywhere.
    const boost::regex SCRUB_MACHINE_ID("\\b(serial[ _]?number|unique_?id|machine_?id|hardware_?id|product_?id|system_?uuid)"
                                        "(\\s*[=:]\\s*)(?!obtained\\b|not available\\b)[^\\s,;\"'<>]+",
                                        boost::regex::icase);
    const boost::regex SCRUB_LOGIN_ID("(['\"](?:mac|id0|hashed_mac|serial_number)['\"]\\s*:\\s*)(?:[a-z]?(['\"])[^'\"]*\\2|[^\\s,}\\]]+)",
                                      boost::regex::icase);
    const boost::regex SCRUB_MAC_ADDR("\\b[0-9A-Fa-f]{2}(?:[:-][0-9A-Fa-f]{2}){5}\\b");
    // Lines from LL's machine-id code: every UUID / long hex run on them is an identifier.
    const boost::regex MACHINE_ID_SOURCE("llmachineid|LLWMIMethods|LLMachineID", boost::regex::icase);
    const boost::regex SCRUB_LONG_HEX("\\b(?:0x)?[0-9A-Fa-f]{12,}\\b");
    const boost::regex SCRUB_HOME("([\\\\/](?:home|Users|users)[\\\\/])[^\\\\/\\s]+");
    const boost::regex SCRUB_EMAIL("\\b[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\\.[A-Za-z]{2,}\\b");

    // Names and avatar keys the viewer knows (self, friends, avatars nearby) to blank out
    // of log lines.
    std::vector<std::string> known_names()
    {
        std::vector<std::string> out;
        if (LLStartUp::getStartupState() < STATE_STARTED) return out;
        uuid_vec_t ids;
        LLWorld::getInstance()->getAvatars(&ids);
        ids.push_back(gAgentID);
        LLAvatarTracker::buddy_map_t buddies;
        LLAvatarTracker::instance().copyBuddyList(buddies);
        for (const auto& b : buddies) ids.push_back(b.first);
        for (const LLUUID& id : ids)
        {
            out.push_back(id.asString());   // avatar keys identify people as much as names do
            LLAvatarName name;
            if (!LLAvatarNameCache::get(id, &name)) continue;   // cached names only, no lookups
            for (const std::string& s : { name.getLegacyName(), name.getUserName(), name.getDisplayName(true) })
            {
                if (s.size() >= 3) out.push_back(s);
            }
        }
        std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    // Masks IPv4 addresses, keeping dotted quads that cannot be one (an octet over 255)
    // or that follow a version cue (viewer versions such as 1.1.0.9 look like addresses).
    std::string scrub_ipv4(const std::string& line)
    {
        std::string out;
        size_t last = 0;
        for (boost::sregex_iterator it(line.begin(), line.end(), SCRUB_IPV4), end; it != end; ++it)
        {
            const boost::smatch& m = *it;
            const size_t pos = (size_t)m.position();
            out.append(line, last, pos - last);
            last = pos + (size_t)m.length();

            bool address = true;
            const std::string quad = m.str(1);
            for (size_t start = 0; start < quad.size() && address; )
            {
                size_t dot = quad.find('.', start);
                if (dot == std::string::npos) dot = quad.size();
                address = std::atoi(quad.substr(start, dot - start).c_str()) <= 255;
                start = dot + 1;
            }
            if (address)
            {
                const size_t from = pos > 40 ? pos - 40 : 0;
                address = !boost::regex_search(line.begin() + from, line.begin() + pos, VERSION_CUE);
            }
            out += address ? std::string("[ip]") : m.str(0);
        }
        out.append(line, last, std::string::npos);
        return out;
    }

    // The OS account name wherever it appears, not only as /home/<user>: install, cache and
    // temp paths often embed it in other shapes (e.g. "/tmp/x/-home-<user>-dir", "D:\<user>\SL").
    // Case-insensitive (Windows paths); names under 3 characters are left alone.
    const std::vector<boost::regex>& os_user_patterns()
    {
        static const std::vector<boost::regex> pats = []()
        {
            std::vector<std::string> names;
            for (const char* var : { "USER", "LOGNAME", "USERNAME" })
            {
                if (const char* v = getenv(var)) names.emplace_back(v);
            }
            for (const char* var : { "HOME", "USERPROFILE" })
            {
                if (const char* v = getenv(var))
                {
                    std::string home(v);
                    while (!home.empty() && (home.back() == '/' || home.back() == '\\')) home.pop_back();
                    const size_t slash = home.find_last_of("/\\");
                    names.emplace_back(slash == std::string::npos ? home : home.substr(slash + 1));
                }
            }
            std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return a.size() > b.size(); });
            names.erase(std::unique(names.begin(), names.end()), names.end());
            std::vector<boost::regex> out;
            for (const std::string& n : names)
            {
                if (n.size() < 3) continue;
                std::string esc;
                for (char c : n)
                {
                    if (std::strchr("\\^$.|?*+()[]{}", c)) esc += '\\';
                    esc += c;
                }
                out.emplace_back(esc, boost::regex::icase);
            }
            return out;
        }();
        return pats;
    }

    std::string scrub_line(std::string line, const std::vector<std::string>& names)
    {
        if (boost::regex_search(line, SDP_LINE))
        {
            // Keep the leading text up to the SDP / ICE payload, which has addresses and credentials.
            boost::smatch m;
            if (boost::regex_search(line, m, SDP_LINE)) line = line.substr(0, (size_t)m.position(2)) + "[sdp removed]";
        }
        if (boost::regex_search(line, AUTH_WORDS)) line = boost::regex_replace(line, SCRUB_UUID, "[uuid]");
        line = boost::regex_replace(line, SCRUB_SECRET_EOL, "$1$2[redacted]");
        line = boost::regex_replace(line, SCRUB_VOICE_KV, "$1$2[redacted]");
        line = boost::regex_replace(line, SCRUB_SIP, "[sip-uri]");
        line = boost::regex_replace(line, SCRUB_USERINFO, "$1[redacted]@");
        line = boost::regex_replace(line, SCRUB_QUERY, "$1[redacted]");
        line = boost::regex_replace(line, SCRUB_KV, "$1$2[redacted]");
        line = boost::regex_replace(line, SCRUB_CAP, "$1[redacted]");
        line = boost::regex_replace(line, SCRUB_URL_PATH, "$1/[path]");
        line = boost::regex_replace(line, SCRUB_EMAIL, "[email]");
        if (boost::regex_search(line, MACHINE_ID_SOURCE))
        {
            line = boost::regex_replace(line, SCRUB_UUID, "[machine-id]");
            line = boost::regex_replace(line, SCRUB_LONG_HEX, "[machine-id]");
        }
        line = boost::regex_replace(line, SCRUB_MACHINE_ID, "$1$2[machine-id]");
        line = boost::regex_replace(line, SCRUB_LOGIN_ID, "$1[machine-id]");
        line = boost::regex_replace(line, SCRUB_MAC_ADDR, "[mac]");
        line = scrub_ipv4(line);
        line = boost::regex_replace(line, SCRUB_IPV6, "[ip]");
        line = boost::regex_replace(line, SCRUB_HOME, "$1[user]");
        for (const boost::regex& u : os_user_patterns())
        {
            line = boost::regex_replace(line, u, "[user]");
        }
        for (const std::string& n : names)
        {
            LLStringUtil::replaceString(line, n, "[name]");
        }
        return line;
    }

    // prio: 0 = DEBUG and high-volume periodic INFO, 1 = other INFO, 2 = warnings, errors
    // and voice / audio / hardware lines. The byte budget is spent on the highest first.
    struct LogEntry { std::string location; std::string text; S32 lines = 0; S32 prio = 1; bool voice = false; bool mem_status = false; };

    // Scrubbed entries from [start, start + length) of the log (every level and source).
    std::vector<LogEntry> read_log_entries(std::ifstream& in, std::streamoff start, std::streamoff length,
                                           const std::vector<std::string>& names)
    {
        std::vector<LogEntry> kept;
        in.clear();
        in.seekg(start);
        std::string line;
        if (start > 0) std::getline(in, line);   // skip the partial first line
        bool keep_entry = false;
        bool chat_entry = false;
        S32 continuation = 0;
        boost::smatch m;
        while ((in.tellg() < start + length) && std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (boost::regex_search(line, m, LOG_HEADER, boost::match_continuous))
            {
                const std::string level = m[1];
                const std::string tags = m[2];
                const std::string source = m[3];
                const size_t header_len = (size_t)m.length(0);
                chat_entry = boost::regex_search(source, CHAT_SOURCES) || boost::regex_search(tags, CHAT_TAGS);
                keep_entry = true;
                continuation = 0;

                std::string text;
                if (chat_entry)
                {
                    // Header only: the message body is what people typed.
                    const size_t sep = line.find(" : ", header_len);
                    text = clip(sep == std::string::npos ? line : line.substr(0, sep), 600) + " : [message removed]";
                }
                else
                {
                    // Timing / renderer summaries (GPU sub-pass marks, frame sections) are one
                    // long line each and the tail is the part that matters: keep them whole.
                    text = clip(line, boost::regex_search(tags, PERF_TAGS) ? PERF_LINE_MAX : 600);
                }
                LogEntry e;
                e.location = source + std::string(m[4]);
                e.lines = 1;
                e.voice = boost::regex_search(line, VOICE_LINES);
                e.mem_status = !chat_entry && boost::regex_search(tags, MEM_STATUS_TAGS);
                if (level == "WARNING" || level == "ERROR" || e.voice || boost::regex_search(line, DEVICE_LINES)) e.prio = 2;
                else if (level == "DEBUG" || boost::regex_search(line, NOISY_LINES)) e.prio = 0;
                e.text = scrub_line(text, names) + "\n";
                kept.push_back(std::move(e));
                continue;
            }
            // Multi-line messages (stack traces, hardware dumps): keep a few lines. A chat
            // message's continuation lines are message text too.
            if (!keep_entry || chat_entry || kept.empty() || ++continuation > 40) continue;
            kept.back().text += scrub_line(clip(line, 600), names) + "\n";
            kept.back().lines++;
        }
        return kept;
    }

    // Pick entries within the byte/line budget: highest priority first, at most 5 per
    // source line (60 for voice lines, which all share one logging call site) so one noisy
    // warning can't crowd out everything else. newest_first: walk from the end.
    std::vector<size_t> pick_log_entries(const std::vector<LogEntry>& entries, bool newest_first,
                                         size_t max_bytes, S32 max_lines)
    {
        std::map<std::string, S32> per_location;
        std::vector<size_t> picked;
        std::vector<char> pinned(entries.size(), 0);
        size_t bytes = 0;
        S32 lines = 0;
        if (newest_first)
        {
            // The latest memory / VRAM status of each reporting call site goes in before the
            // budget is spent (#739): a long log full of warnings must not push them out.
            size_t pins = 0;
            for (size_t n = 0; n < entries.size() && pins < 24; ++n)
            {
                const size_t i = entries.size() - 1 - n;
                const LogEntry& e = entries[i];
                if (!e.mem_status || e.text.size() > 4096 || e.lines > 12) continue;
                if (per_location.count(e.location)) continue;
                per_location[e.location] = 1;
                pinned[i] = 1;
                bytes += e.text.size();
                lines += e.lines;
                picked.push_back(i);
                ++pins;
            }
        }
        for (S32 prio = 2; prio >= 0; --prio)
        {
            for (size_t n = 0; n < entries.size(); ++n)
            {
                const size_t i = newest_first ? entries.size() - 1 - n : n;
                const LogEntry& e = entries[i];
                if (e.prio != prio || pinned[i]) continue;
                if (++per_location[e.location] > (e.voice ? 60 : 5)) continue;
                if (bytes + e.text.size() > max_bytes || lines + e.lines > max_lines) break;
                bytes += e.text.size();
                lines += e.lines;
                picked.push_back(i);
            }
        }
        std::sort(picked.begin(), picked.end());
        return picked;
    }

    // The start of the session (hardware, Vulkan and settings bring-up) plus the end
    // (what happened last), budgeted by priority (warnings, errors, voice and hardware first) and scrubbed.
    // tail_only (#790, an earlier session's log): its last tail_read bytes only, no head.
    std::string build_log_excerpt(const std::string& path, S32& lines_out, size_t max_bytes = LOG_MAX_BYTES,
                                  S32 max_lines = LOG_MAX_LINES, bool tail_only = false,
                                  size_t tail_read = LOG_READ_TAIL, std::streamoff end_at = -1)
    {
        lines_out = 0;
        std::ifstream in(path, std::ios::binary);
        if (!in) return std::string();
        in.seekg(0, std::ios::end);
        std::streamoff size = in.tellg();
        if (end_at > 0 && end_at < size) size = end_at;   // the excerpt ends here, not at the file's end
        if (size <= 0) return std::string();
        const std::vector<std::string> names = known_names();
        const std::streamoff head_len = tail_only ? 0 : 1024 * 1024;
        const std::streamoff tail_len = (std::streamoff)tail_read;

        std::vector<LogEntry> head, tail;
        if (size <= head_len + tail_len)
        {
            tail = read_log_entries(in, 0, size, names);
        }
        else
        {
            head = read_log_entries(in, 0, head_len, names);
            tail = read_log_entries(in, size - tail_len, tail_len, names);
        }
        // Whole-file case: the head is the first entries of the same list.
        const bool shared = head.empty();
        const std::vector<LogEntry>& head_src = shared ? tail : head;
        std::vector<size_t> head_pick;
        if (!tail_only) head_pick = pick_log_entries(head_src, false, max_bytes / 4, max_lines / 4);
        size_t head_bytes = 0;
        S32 head_lines = 0;
        for (size_t i : head_pick) { head_bytes += head_src[i].text.size(); head_lines += head_src[i].lines; }
        std::vector<size_t> tail_pick = pick_log_entries(tail, true, max_bytes - std::min(max_bytes, head_bytes),
                                                         max_lines - std::min(max_lines, head_lines));
        std::string out;
        size_t last = 0;
        bool any = false;
        for (size_t i : head_pick)
        {
            out += head_src[i].text;
            lines_out += head_src[i].lines;
            last = i;
            any = true;
        }
        bool gap_marked = false;
        for (size_t i : tail_pick)
        {
            if (shared && any && i <= last) continue;   // already in the head part
            if (!gap_marked && any)
            {
                out += "...\n";
                gap_marked = true;
            }
            out += tail[i].text;
            lines_out += tail[i].lines;
        }
        return out;
    }

    // ---- #790: earlier sessions that ended in a crash / graphics device loss ----

    // The session in `path` began right after a crash or a device loss of its own
    // predecessor (the lines the startup checks write; read from the head of the log).
    bool log_says_previous_ended_badly(const std::string& path, bool* device_lost)
    {
        if (device_lost) *device_lost = false;
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        std::string head(256 * 1024, '\0');
        in.read(&head[0], (std::streamsize)head.size());
        head.resize((size_t)std::max<std::streamsize>(0, in.gcount()));
        const bool lost = head.find("previous session closed because the graphics device was lost") != std::string::npos
                       || head.find("graphics device lost): its log is kept") != std::string::npos;
        if (device_lost) *device_lost = lost;
        return lost || head.find("last exec crashed") != std::string::npos
            || head.find("Previous session did not end cleanly") != std::string::npos
            || head.find("setting LastExecEvent to") != std::string::npos;
    }

    // The session in `path` was disconnected from the grid: the offset just past the end of
    // its last PRE-DISCONNECT block (LLMemDiag::logPreDisconnect), or -1. A disconnect quits
    // cleanly, so the user reports from the next session and the block sits in the old log,
    // often followed by minutes of the disconnect dialog's own logging.
    std::streamoff disconnect_block_end(const std::string& path)
    {
        constexpr std::streamoff SCAN = 16 * 1024 * 1024;
        std::ifstream in(path, std::ios::binary);
        if (!in) return -1;
        in.seekg(0, std::ios::end);
        const std::streamoff size = in.tellg();
        if (size <= 0) return -1;
        const std::streamoff start = std::max<std::streamoff>(0, size - SCAN);
        std::string buf((size_t)(size - start), '\0');
        in.seekg(start);
        in.read(&buf[0], (std::streamsize)buf.size());
        buf.resize((size_t)std::max<std::streamsize>(0, in.gcount()));
        const size_t begin = buf.rfind("==== PRE-DISCONNECT (");
        if (begin == std::string::npos) return -1;
        size_t end = buf.find("==== END PRE-DISCONNECT ====", begin);
        if (end == std::string::npos) return -1;
        end = buf.find('\n', end);
        return start + (std::streamoff)(end == std::string::npos ? buf.size() : end + 1);
    }

    // "Slipstream-crash-YYYYMMDD-HHMMSS.log" for a file's modification time (keepCrashLog's name).
    std::string crash_log_name_for(const std::string& path)
    {
        llstat st;
        if (LLFile::stat(path, &st) != 0) return std::string();
        time_t t = st.st_mtime;
        struct tm tmv;
#if LL_WINDOWS
        localtime_s(&tmv, &t);
#else
        localtime_r(&t, &tmv);
#endif
        char stamp[32];
        strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &tmv);
        return std::string("Slipstream-crash-") + stamp + ".log";
    }

    // The kept log (keepCrashLog) of the session BEFORE the one in `old_log`: the newest
    // Slipstream-crash-*.log older than old_log's own copy. Empty when none was kept.
    std::string predecessor_crash_log(const std::string& old_log)
    {
        const std::string own = crash_log_name_for(old_log);
        if (own.empty()) return std::string();
        std::vector<std::string> kept;
        LLDirIterator iter(gDirUtilp->getExpandedFilename(LL_PATH_LOGS, ""), "Slipstream-crash-*.log");
        std::string found;
        while (iter.next(found))
        {
            if (found < own) kept.push_back(found);
        }
        if (kept.empty()) return std::string();
        std::sort(kept.begin(), kept.end());
        // Only a log from shortly before (24 h): an old leftover is not "the session before"
        // (Beta 10 and earlier never kept a device-lost session's log at all).
        auto when = [](const std::string& name) -> time_t
        {
            int Y = 0, M = 0, D = 0, h = 0, m = 0, sec = 0;
            if (std::sscanf(name.c_str(), "Slipstream-crash-%4d%2d%2d-%2d%2d%2d", &Y, &M, &D, &h, &m, &sec) != 6) return 0;
            struct tm tmv{};
            tmv.tm_year = Y - 1900; tmv.tm_mon = M - 1; tmv.tm_mday = D;
            tmv.tm_hour = h; tmv.tm_min = m; tmv.tm_sec = sec; tmv.tm_isdst = -1;
            return mktime(&tmv);
        };
        const time_t t_own = when(own), t_prev = when(kept.back());
        if (!t_own || !t_prev || t_own < t_prev || t_own - t_prev > 24 * 3600) return std::string();
        return gDirUtilp->getExpandedFilename(LL_PATH_LOGS, kept.back());
    }

    // The last device-loss report (slipstream_gpu_lost_last.txt: what failed, the GPU
    // breadcrumbs / fault info, the viewer state), scrubbed and budgeted.
    std::string gpu_lost_report_excerpt(S32& lines_out)
    {
        lines_out = 0;
        const S32 slot = LLAppViewer::instance() ? LLAppViewer::instance()->getInstanceSlot() : 1;
        const std::string name = slot > 1 ? llformat("slipstream_gpu_lost_last-%d.txt", slot)
                                          : std::string("slipstream_gpu_lost_last.txt");
        std::ifstream in(gDirUtilp->getExpandedFilename(LL_PATH_LOGS, name), std::ios::binary);
        if (!in) return std::string();
        const std::vector<std::string> names = known_names();
        std::string out, line;
        while (lines_out < GPU_LOST_MAX_LINES && std::getline(in, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const std::string t = scrub_line(clip(line, 600), names) + "\n";
            if (out.size() + t.size() > GPU_LOST_MAX_BYTES) break;
            out += t;
            ++lines_out;
        }
        return out;
    }

    void indent_into(std::ostringstream& out, const std::string& text)
    {
        std::istringstream in(text);
        std::string line;
        while (std::getline(in, line)) out << "    " << line << "\n";
    }

    bool sCrashOffered = false;

    // #966: a performance capture attached to the log field (the relay takes 256 KB / 4000
    // lines in all; the excerpts above stay under 192 KB / ~2300 lines).
    constexpr size_t PERF_MAX_BYTES = 48 * 1024;
    // A capture this recent is attached by default.
    constexpr F64 PERF_RECENT_SEC = 15.0 * 60.0;
}

// ---------------------------------------------------------------------------------

// static
std::string LLFloaterReportBug::scrubText(const std::string& text)
{
    const std::vector<std::string> names = known_names();
    std::istringstream in(text);
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) out << scrub_line(line, names) << "\n";
    return out.str();
}

LLFloaterReportBug::LLFloaterReportBug(const LLSD& key)
:   LLFloater(key)
{
}

bool LLFloaterReportBug::postBuild()
{
    mTabs        = getChild<LLTabContainer>("tabs");
    mSummary     = getChild<LLLineEditor>("summary");
    mDescription = getChild<LLTextEditor>("description");
    mSteps       = getChild<LLTextEditor>("steps");
    mAvatar      = getChild<LLLineEditor>("avatar");
    mSystemInfo  = getChild<LLTextEditor>("system_info");
    mAttachLog   = getChild<LLCheckBoxCtrl>("attach_log");
    mAttachShot  = getChild<LLCheckBoxCtrl>("attach_screenshot");
    mShotUI      = getChild<LLCheckBoxCtrl>("screenshot_ui");
    mRetakeBtn   = getChild<LLButton>("retake_btn");
    mPreviewArea = getChild<LLView>("screenshot_preview");
    mShotNote    = getChild<LLTextBox>("screenshot_note");
    mPayloadView = getChild<LLTextEditor>("payload");
    mStatus      = getChild<LLTextBox>("status_text");
    mSendBtn     = getChild<LLButton>("send_btn");
    mCloseBtn    = getChild<LLButton>("close_btn");

    mSummary->setMaxTextChars(SUMMARY_MAX);
    mAvatar->setMaxTextChars(AVATAR_MAX);
    mDescription->setMaxTextLength(DESCRIPTION_MAX);
    mSteps->setMaxTextLength(STEPS_MAX);

    mTabs->setCommitCallback(boost::bind(&LLFloaterReportBug::onTabChanged, this));
    mSendBtn->setCommitCallback(boost::bind(&LLFloaterReportBug::onSend, this));
    mCloseBtn->setCommitCallback(boost::bind(&LLFloater::closeFloater, this, false));
    mRetakeBtn->setCommitCallback(boost::bind(&LLFloaterReportBug::takeScreenshot, this));
    mAttachLog->setCommitCallback(boost::bind(&LLFloaterReportBug::updateControls, this));
    mAttachShot->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        if (mAttachShot->get() && mShotJpeg.empty()) takeScreenshot();
        updateControls();
    });
    mShotUI->setCommitCallback(boost::bind(&LLFloaterReportBug::takeScreenshot, this));
    // #966 performance reports
    mAttachPerf     = getChild<LLCheckBoxCtrl>("attach_perf");
    mPerfCaptureBtn = getChild<LLButton>("perf_capture_btn");
    mPerfNote       = getChild<LLTextBox>("perf_note");
    mAttachPerf->setCommitCallback([this](LLUICtrl*, const LLSD&)
    {
        // Ticked with no capture yet: offer one (the button), never start it unasked.
        updatePerfControls();
    });
    mPerfCaptureBtn->setCommitCallback(boost::bind(&LLFloaterReportBug::onPerfCapture, this));
    mPerfConn = LLVKPerfCapture::instance().setDoneCallback(boost::bind(&LLFloaterReportBug::onPerfDone, this));
    getChild<LLTextBox>("tracker_link")->setTextArg("[URL]", gSavedSettings.getString("ReportBugURL"));

    mMyReports       = getChild<LLScrollListCtrl>("my_reports");
    mFollowText      = getChild<LLTextEditor>("followup_comment");
    mFollowSendBtn   = getChild<LLButton>("followup_send_btn");
    mFollowOpenBtn   = getChild<LLButton>("followup_open_btn");
    mFollowForgetBtn = getChild<LLButton>("followup_forget_btn");
    mFollowText->setMaxTextLength(FOLLOWUP_MAX);
    mMyReports->setCommitCallback(boost::bind(&LLFloaterReportBug::updateFollowupControls, this));
    mFollowSendBtn->setCommitCallback(boost::bind(&LLFloaterReportBug::onFollowupSend, this));
    mFollowOpenBtn->setCommitCallback(boost::bind(&LLFloaterReportBug::onFollowupOpen, this));
    mFollowForgetBtn->setCommitCallback(boost::bind(&LLFloaterReportBug::onFollowupForget, this));
    mTrackerConn = LLReportTracker::instance().setChangedCallback(boost::bind(&LLFloaterReportBug::refreshMyReports, this));
    refreshMyReports();
    return true;
}

void LLFloaterReportBug::onOpen(const LLSD& key)
{
    if (mSending)
    {
        return;   // keep the in-flight state visible
    }
    if (mSent)
    {
        // The floater is single-instance and reused, so closing it only hides it: after a
        // report was filed, opening it again starts a new report instead of showing the
        // old result with every field disabled for the rest of the session.
        mSent = false;
        mSummary->clear();
        mDescription->clear();
        mSteps->clear();
        mShotJpeg.clear();
    }
    mStatus->setText(LLStringUtil::null);
    refreshMyReports();
    const bool in_world = LLStartUp::getStartupState() >= STATE_STARTED;
    if (in_world && mAvatar->getText().empty())
    {
        LLAvatarName name;
        if (LLAvatarNameCache::get(gAgentID, &name))
        {
            mAvatar->setText(name.getLegacyName());
        }
    }
    refreshSystemInfo();
    refreshLog();
    mAttachShot->setEnabled(in_world);
    if (!in_world)
    {
        mAttachShot->set(false);
    }
    if (mAttachShot->get() && mCrashEvent.empty())
    {
        takeScreenshot();   // the scene as it is when the window opens
    }
    {
        // #966: a recent performance capture is attached by default (shown on WHAT WILL BE SENT).
        const LLVKPerfCapture& pc = LLVKPerfCapture::instance();
        if (!pc.lastText().empty() && LLTimer::getTotalSeconds() - pc.lastAt() < PERF_RECENT_SEC && mCrashEvent.empty())
        {
            mAttachPerf->set(true);
        }
    }
    updateControls();
    mSummary->setFocus(true);
}

bool LLFloaterReportBug::attachPerf() const
{
    return mAttachPerf && mAttachPerf->get() && !LLVKPerfCapture::instance().lastText().empty();
}

void LLFloaterReportBug::onPerfCapture()
{
    LLVKPerfCapture& pc = LLVKPerfCapture::instance();
    if (pc.capturing()) return;
    static LLCachedControl<U32> s_seconds(gSavedSettings, "PerfCaptureSeconds", LLVKPerfCapture::kDefaultSeconds);
    pc.startCapture(s_seconds, false);   // (not the clipboard: the report attaches it)
    mPerfRequested = true;
    mAttachPerf->set(true);
    updatePerfControls();
}

void LLFloaterReportBug::onPerfDone()
{
    if (mPerfRequested && !LLVKPerfCapture::instance().lastText().empty()) mAttachPerf->set(true);
    mPerfRequested = false;
    updatePerfControls();
    if (mTabs && mTabs->getCurrentPanel() && mTabs->getCurrentPanel()->getName() == "payload_panel") refreshPayloadView();
}

void LLFloaterReportBug::updatePerfControls()
{
    if (!mAttachPerf) return;
    const LLVKPerfCapture& pc = LLVKPerfCapture::instance();
    const bool in_world = LLStartUp::getStartupState() >= STATE_STARTED;
    const bool editable = !mSending && !mSent;
    mAttachPerf->setEnabled(editable);
    mPerfCaptureBtn->setEnabled(editable && in_world && !pc.capturing());
    std::string note;
    LLStringUtil::format_map_t args;
    if (pc.capturing())
    {
        args["[SECONDS]"] = llformat("%.0f", (F64)pc.captureRemaining());
        note = getString("perf_capturing", args);
    }
    else if (!pc.lastText().empty())
    {
        args["[SUMMARY]"] = pc.lastSummary();
        args["[MINUTES]"] = llformat("%.0f", (LLTimer::getTotalSeconds() - pc.lastAt()) / 60.0);
        note = getString(mAttachPerf->get() ? "perf_attached" : "perf_available", args);
    }
    else
    {
        note = getString(!in_world ? "perf_login" : (mAttachPerf->get() ? "perf_none_ticked" : "perf_none"));
    }
    if (note != mPerfNoteText)
    {
        mPerfNoteText = note;
        mPerfNote->setText(note);
    }
}

void LLFloaterReportBug::setCrashMode()
{
    mCrashEvent = exec_event_name(gLastExecEvent);
    if (mCrashEvent.empty()) mCrashEvent = "crashed";
    mCrashEvent = "Previous session " + mCrashEvent;
    if (gLastExecDuration > 0)
    {
        mCrashEvent += llformat(" after %d s", (S32)gLastExecDuration);
    }
    mAttachShot->set(false);
    mAttachLog->set(true);
    mAttachPerf->set(false);   // (#966: a capture of this session says nothing about that crash)
    if (mSummary->getText().empty())
    {
        mSummary->setText(std::string("Crash: "));
    }
    refreshSystemInfo();
    refreshLog();
    updateControls();
}

void LLFloaterReportBug::refreshSystemInfo()
{
    const LLSD info = LLAppViewer::instance()->getViewerInfo();
    LLSD sys = LLSD::emptyMap();
    auto put = [&sys](const char* key, const std::string& value)
    {
        std::string v = value;
        LLStringUtil::trim(v);
        if (!v.empty()) sys[key] = clip(v, SYSTEM_VALUE_MAX);
    };
    put("version", info["VIEWER_VERSION_STR"].asString());
    put("channel", info["CHANNEL"].asString());
    put("os", info["OS_VERSION"].asString());
    put("cpu", info["CPU"].asString());
    if (info.has("MEMORY_MB")) put("memory", llformat("%d MB", info["MEMORY_MB"].asInteger()));
    put("gpu", info["GRAPHICS_CARD"].asString());
    std::string driver = info["GRAPHICS_DRIVER_VERSION"].asString();
    if (driver.empty())
    {
        driver = gGPUInfo.mDriverVersion.empty() ? query_vulkan_driver(gGPUInfo.mRenderer) : gGPUInfo.mDriverVersion;
    }
    put("gpu_driver", driver);
    if (info.has("VULKAN_VERSION"))
    {
        put("vulkan", info["VULKAN_VERSION"].asString() + "; ray tracing: " + info["RAY_TRACING"].asString());
    }
    // VRAM budget + accounting (#405 / #407): where the memory goes on this machine.
    put("vram", LLVulkanContext::instance().vramStatusText(0));
    put("vram_categories", LLVulkanContext::instance().vramStatusText(1));
    // #407: what the untracked part is (driver page padding / driver-internal / rest) and
    // the display it was measured on (swapchain, monitor count, mode, HDR), so reports
    // from other machines give the overhead per vendor / driver / OS / resolution.
    put("vram_overhead", LLVulkanContext::instance().vramStatusText(3));
    // #411: texture suballocation -- pool blocks, occupancy, slack, allocation rate.
    {
        const std::string pools = LLVulkanContext::instance().vramStatusText(4);
        if (!pools.empty()) put("vram_texpools", pools);
    }
    // #458: host-visible memory by category + allocation traffic (commit exhaustion).
    put("hostmem", LLVulkanContext::instance().hostMemText());
    // #700: memory pressure state and allocation failures ("things aren't loading" reports).
    put("mem_status", LLMemDiag::statusLine());
    put("alloc_failures", LLMemDiag::allocFailText() + " | " + LLMemoryGovernor::allocFailString());   // (#698)
    // #471: the heaviest pipeline-compile batch (wall time, peak memory growth, slowest).
    put("pipelines", LLVulkanContext::pipelineCompileText());
    put("display", LLVulkanContext::instance().displayInfoText());
    // #529: what this session downloaded, by category (each value fits the relay limit)
    put("data_usage", SLDataUsage::reportText(0));
    put("data_usage_http", SLDataUsage::reportText(1));
    put("data_usage_udp", SLDataUsage::reportText(2));
    put("data_usage_cache", SLDataUsage::reportText(3));
    put("build_config", info["BUILD_CONFIG"].asString());
    put("window", llformat("%dx%d", info["WINDOW_WIDTH"].asInteger(), info["WINDOW_HEIGHT"].asInteger()));
    if (LLStartUp::getStartupState() >= STATE_STARTED)
    {
        put("region", info["REGION"].asString());   // RLVa-aware (hidden under @showloc)
        put("grid", LLGridManager::getInstance()->getGridLabel());
    }
    LLSD settings = LLSD::emptyMap();
    for (const char* name : REPORT_SETTINGS)
    {
        LLControlVariable* control = gSavedSettings.getControl(name);
        if (!control) continue;
        const LLSD value = control->getValue();
        std::string text = value.isBoolean() ? (value.asBoolean() ? "true" : "false")
                         : value.isReal()    ? llformat("%g", value.asReal())
                                             : value.asString();
        settings[name] = clip(text, SYSTEM_VALUE_MAX);
    }
    sys["settings"] = settings;
    // RenderVK* settings that differ from their defaults, in chunks that fit the relay's
    // per-value limit (at most 3 chunks; the list ends in "+N more" when it was cut).
    {
        const std::string list = nonDefaultRenderSettings(24);
        S32 chunk = 0;
        size_t pos = 0;
        while (pos < list.size() && chunk < 3)
        {
            size_t end = list.size();
            if (end - pos > SYSTEM_VALUE_MAX)
            {
                end = list.rfind(' ', pos + SYSTEM_VALUE_MAX);
                if (end == std::string::npos || end <= pos) end = pos + SYSTEM_VALUE_MAX;
            }
            sys[chunk == 0 ? "non_default_render" : llformat("non_default_render_%d", chunk + 1)] = list.substr(pos, end - pos);
            pos = end + 1;
            ++chunk;
        }
        if (list.empty()) sys["non_default_render"] = "none";
    }
    mSystem = sys;

    std::ostringstream out;
    static const std::pair<const char*, const char*> LABELS[] = {
        { "version", "Version" }, { "channel", "Channel" }, { "os", "OS" }, { "cpu", "CPU" },
        { "memory", "Memory" }, { "gpu", "GPU" }, { "gpu_driver", "GPU driver" }, { "vulkan", "Vulkan" },
        { "vram", "VRAM" }, { "vram_categories", "VRAM by category" }, { "vram_overhead", "VRAM overhead" },
        { "hostmem", "Host memory" }, { "mem_status", "Memory state" },
        { "alloc_failures", "Allocation failures" }, { "pipelines", "Pipeline compiles" }, { "display", "Display" },
        { "data_usage", "Data usage" }, { "data_usage_http", "Data HTTP" }, { "data_usage_udp", "Data UDP" },
        { "data_usage_cache", "Data caches" },
        { "non_default_render", "Non-default render settings" }, { "non_default_render_2", "  (continued)" },
        { "non_default_render_3", "  (continued)" },
        { "window", "Window" }, { "build_config", "Build" }, { "grid", "Grid" }, { "region", "Region" } };
    for (const auto& [key, label] : LABELS)
    {
        if (mSystem.has(key)) out << label << ": " << mSystem[key].asString() << "\n";
    }
    out << "Settings:";
    for (const auto& [key, value] : llsd::inMap(mSystem["settings"]))
    {
        out << " " << key << "=" << value.asString() << ";";
    }
    if (!mCrashEvent.empty()) out << "\n" << mCrashEvent;
    mSystemInfo->setText(out.str());
}

void LLFloaterReportBug::refreshLog()
{
    // This viewer's own log (Slipstream.log, or Slipstream-N.log for a further viewer). After
    // a crash the crashed session's log was rotated to <name>.old at startup.
    LLAppViewer* app = LLAppViewer::instance();
    std::string path = mCrashEvent.empty() ? LLError::logFileName() : app->getOldLogFilePath();
    if (path.empty()) path = app->getLogFilePath();
    // #790: the session that actually failed is often NOT the one in `path`. A device loss
    // closes the viewer cleanly (no crash prompt: the user reports from the next session),
    // and a crash right after it (driver reset) pushes the failed session one log further
    // back. Add the end of that earlier session's log and the device-loss report.
    std::string prev_label, prev_path;
    std::streamoff prev_end = -1;
    bool gpu_lost = false;
    if (mCrashEvent.empty())
    {
        if (gLastExecEvent != LAST_EXEC_NORMAL || gLastExecDeviceLost)
        {
            prev_path = app->getOldLogFilePath();
            prev_label = gLastExecDeviceLost ? "previous session (graphics device lost)" : "previous session (crashed)";
            gpu_lost = gLastExecDeviceLost;
        }
        else
        {
            // A grid disconnect also ends cleanly: include the lead-up to it from the old log.
            const std::string old_log = app->getOldLogFilePath();
            prev_end = old_log.empty() ? -1 : disconnect_block_end(old_log);
            if (prev_end > 0)
            {
                prev_path = old_log;
                prev_label = "previous session (disconnected from the grid)";
            }
        }
    }
    else
    {
        bool before_lost = false;
        if (log_says_previous_ended_badly(path, &before_lost))
        {
            prev_path = predecessor_crash_log(path);
            prev_label = before_lost ? "the session before it (graphics device lost)"
                                     : "the session before it (crashed)";
        }
        gpu_lost = gLastExecDeviceLost || before_lost;
    }
    S32 prev_lines = 0, lost_lines = 0;
    const std::string prev = prev_path.empty() ? std::string()
        : build_log_excerpt(prev_path, prev_lines, PREV_LOG_MAX_BYTES, PREV_LOG_MAX_LINES, true, PREV_LOG_READ_TAIL,
                            prev_end);
    const std::string lost = gpu_lost ? gpu_lost_report_excerpt(lost_lines) : std::string();
    const size_t main_bytes = LOG_MAX_BYTES - (prev.empty() ? 0 : PREV_LOG_MAX_BYTES) - (lost.empty() ? 0 : GPU_LOST_MAX_BYTES);
    mLogExcerpt = build_log_excerpt(path, mLogLines, main_bytes, LOG_MAX_LINES);
    if (!prev.empty())
    {
        mLogExcerpt += "\n--- " + prev_label + ": end of its log ---\n" + prev;
        mLogLines += prev_lines + 2;
    }
    if (!lost.empty())
    {
        mLogExcerpt += "\n--- graphics device loss report (slipstream_gpu_lost_last.txt) ---\n" + lost;
        mLogLines += lost_lines + 2;
    }
    mAttachLog->setLabelArg("[LINES]", llformat("%d", mLogLines));
}

void LLFloaterReportBug::takeScreenshot()
{
    mShotJpeg.clear();
    mShotRaw = nullptr;
    mShotTexture = nullptr;
    if (LLStartUp::getStartupState() < STATE_STARTED || !gViewerWindow)
    {
        updateControls();
        return;
    }
    const S32 win_w = gViewerWindow->getWindowWidthRaw();
    const S32 win_h = gViewerWindow->getWindowHeightRaw();
    if (win_w <= 0 || win_h <= 0) return;
    const F32 scale = llmin(1.f, (F32)SHOT_MAX_WIDTH / win_w);
    const S32 w = llmax(64, (S32)(win_w * scale));
    const S32 h = llmax(64, (S32)(win_h * scale));
    const bool with_ui = mShotUI->get();

    LLPointer<LLImageRaw> raw = new LLImageRaw;
    // The interface capture includes whatever windows are open, but never this one.
    if (with_ui) setVisible(false);
    const bool ok = gViewerWindow->rawSnapshot(raw, w, h, true, false, with_ui, true, false, false,
                                               false /* hide L$ balance */);
    if (with_ui) setVisible(true);
    if (!ok || raw->isBufferInvalid() || raw->getWidth() < 16 || raw->getHeight() < 16)
    {
        LL_WARNS("ReportBug") << "Screenshot capture failed" << LL_ENDL;
        mShotNote->setText(getString("screenshot_failed"));
        updateControls();
        return;
    }
    if (raw->getWidth() > 2560 || raw->getHeight() > 2560)
    {
        const F32 f = llmin(2560.f / raw->getWidth(), 2560.f / raw->getHeight());
        raw->scale((S32)(raw->getWidth() * f), (S32)(raw->getHeight() * f));
    }

    for (S32 quality : { 85, 70, 50 })
    {
        LLPointer<LLImageJPEG> jpeg = new LLImageJPEG(quality);
        if (!jpeg->encode(raw, 0.f) || !jpeg->getData()) break;
        if ((size_t)jpeg->getDataSize() <= SHOT_MAX_BYTES)
        {
            mShotJpeg.assign(jpeg->getData(), jpeg->getData() + jpeg->getDataSize());
            break;
        }
    }
    if (mShotJpeg.empty())
    {
        mShotNote->setText(getString("screenshot_failed"));
        updateControls();
        return;
    }
    mShotRaw = raw;

    // Preview texture: a power-of-two copy, drawn back at the capture's aspect ratio.
    LLPointer<LLImageRaw> preview = new LLImageRaw(raw->getData(), (U16)raw->getWidth(), (U16)raw->getHeight(),
                                                   raw->getComponents());
    if (preview->scale(512, 256))
    {
        mShotTexture = LLViewerTextureManager::getLocalTexture(preview.get(), false);
    }
    LLStringUtil::format_map_t args;
    args["[WIDTH]"] = llformat("%d", raw->getWidth());
    args["[HEIGHT]"] = llformat("%d", raw->getHeight());
    args["[KB]"] = llformat("%d", (S32)(mShotJpeg.size() / 1024));
    mShotNote->setText(getString(with_ui ? "screenshot_info_ui" : "screenshot_info", args));
    updateControls();
}

void LLFloaterReportBug::updateControls()
{
    const bool in_world = LLStartUp::getStartupState() >= STATE_STARTED;
    const bool editable = !mSending && !mSent;
    mSummary->setEnabled(editable);
    mDescription->setEnabled(editable);
    mSteps->setEnabled(editable);
    mAvatar->setEnabled(editable);
    mAttachLog->setEnabled(editable && !mLogExcerpt.empty());
    mAttachShot->setEnabled(editable && in_world);
    mShotUI->setEnabled(editable && in_world && mAttachShot->get());
    mRetakeBtn->setEnabled(editable && in_world && mAttachShot->get());
    if (!in_world)
    {
        mShotNote->setText(getString("screenshot_login"));
    }
    else if (!mAttachShot->get())
    {
        mShotNote->setText(getString("screenshot_off"));
    }
    mSendBtn->setEnabled(editable);
    mCloseBtn->setLabel(getString(mSent ? "close_label" : "cancel_label"));
    updatePerfControls();
}

void LLFloaterReportBug::draw()
{
    if (LLVKPerfCapture::instance().capturing()) updatePerfControls();   // (the countdown)
    LLFloater::draw();
    if (mShotTexture && mShotRaw && mAttachShot->get() && mPreviewArea->isInVisibleChain())
    {
        LLRect area;
        mPreviewArea->localRectToOtherView(mPreviewArea->getLocalRect(), &area, this);
        const F32 aspect = (F32)mShotRaw->getWidth() / (F32)mShotRaw->getHeight();
        S32 w = area.getWidth(), h = area.getHeight();
        if ((F32)w / h > aspect) w = (S32)(h * aspect); else h = (S32)(w / aspect);
        const S32 x = area.mLeft + (area.getWidth() - w) / 2;
        const S32 y = area.mBottom + (area.getHeight() - h) / 2;
        gl_draw_scaled_image(x, y, w, h, mShotTexture);
    }
}

bool LLFloaterReportBug::validate(std::string& error) const
{
    std::string summary = mSummary->getText();
    std::string description = mDescription->getText();
    LLStringUtil::trim(summary);
    LLStringUtil::trim(description);
    if ((S32)utf8str_to_wstring(summary).size() < SUMMARY_MIN || summary == "Crash:")
    {
        error = getString("error_summary");
        return false;
    }
    if ((S32)utf8str_to_wstring(description).size() < DESCRIPTION_MIN)
    {
        error = getString("error_description");
        return false;
    }
    return true;
}

LLSD LLFloaterReportBug::buildPayload() const
{
    auto trimmed = [](std::string s) { LLStringUtil::trim(s); return s; };
    LLSD payload = LLSD::emptyMap();
    payload["schema"] = 1;
    payload["summary"] = trimmed(mSummary->getText());
    payload["description"] = trimmed(mDescription->getText());
    payload["steps"] = trimmed(mSteps->getText());
    payload["avatar"] = trimmed(mAvatar->getText());
    if (!mCrashEvent.empty()) payload["crash_event"] = clip(mCrashEvent, 64);
    payload["system"] = mSystem;
    std::string log;
    if (mAttachLog->get() && !mLogExcerpt.empty()) log = mLogExcerpt;
    if (attachPerf())
    {
        // #966: the relay takes no extra top-level field, so the capture rides in the log
        // text (the issue's log attachment), after the excerpt. Already scrubbed.
        const LLVKPerfCapture& pc = LLVKPerfCapture::instance();
        if (!log.empty()) log += "\n";
        log += "--- performance capture (" + pc.lastSummary() + ") ---\n" + clip(pc.lastText(), PERF_MAX_BYTES);
    }
    if (!log.empty()) payload["log"] = log;
    return payload;
}

std::string LLFloaterReportBug::describePayload(const LLSD& payload) const
{
    std::ostringstream out;
    out << "POST " << gSavedSettings.getString("ReportBugRelayURL") << "\n"
        << "X-Slipstream-Version: " << LLVersionInfo::instance().getChannelAndVersion() << "\n\n";
    out << "summary: " << payload["summary"].asString() << "\n";
    out << "avatar: " << (payload["avatar"].asString().empty() ? std::string("(none)") : payload["avatar"].asString()) << "\n";
    if (payload.has("crash_event")) out << "crash_event: " << payload["crash_event"].asString() << "\n";
    out << "description:\n";
    indent_into(out, payload["description"].asString());
    out << "steps:\n";
    indent_into(out, payload["steps"].asString());
    out << "system:\n";
    for (const auto& [key, value] : llsd::inMap(payload["system"]))
    {
        if (key == "settings") continue;
        out << "    " << key << ": " << value.asString() << "\n";
    }
    out << "    settings:\n";
    for (const auto& [key, value] : llsd::inMap(payload["system"]["settings"]))
    {
        out << "        " << key << ": " << value.asString() << "\n";
    }
    if (mAttachShot->get() && !mShotJpeg.empty())
    {
        out << "screenshot: JPEG " << mShotRaw->getWidth() << "x" << mShotRaw->getHeight() << ", "
            << mShotJpeg.size() / 1024 << " KB (the image shown on the Report tab)\n";
    }
    else
    {
        out << "screenshot: (none)\n";
    }
    if (attachPerf())
    {
        out << "performance capture: " << LLVKPerfCapture::instance().lastSummary() << " (at the end of the log)\n";
    }
    if (payload.has("log"))
    {
        const std::string& log = payload["log"].asStringRef();
        out << "log (" << std::count(log.begin(), log.end(), '\n') << " lines):\n" << log;
    }
    else
    {
        out << "log: (none)\n";
    }
    return out.str();
}

void LLFloaterReportBug::refreshPayloadView()
{
    mPayloadView->setText(describePayload(buildPayload()));
}

void LLFloaterReportBug::onTabChanged()
{
    if (mTabs->getCurrentPanel() && mTabs->getCurrentPanel()->getName() == "payload_panel")
    {
        refreshPayloadView();
    }
}

void LLFloaterReportBug::onSend()
{
    if (mSending || mSent) return;
    std::string error;
    if (!validate(error))
    {
        mStatus->setText(error);
        return;
    }
    LLSD payload = buildPayload();
    const std::string trimmed_summary = payload["summary"].asString();
    if (mAttachShot->get() && !mShotJpeg.empty())
    {
        payload["screenshot"]["type"] = "image/jpeg";
        payload["screenshot"]["data"] = LLBase64::encode(mShotJpeg.data(), mShotJpeg.size());
    }
    const std::string body = valid_utf8(boost::json::serialize(LlsdToJson(payload)));
    const std::string url = gSavedSettings.getString("ReportBugRelayURL");
    mSending = true;
    mStatus->setText(getString("sending"));
    updateControls();
    LL_INFOS("ReportBug") << "Sending bug report (" << body.size() << " bytes)" << LL_ENDL;
    LLCoros::instance().launch("LLFloaterReportBug::sendCoro",
        boost::bind(&LLFloaterReportBug::sendCoro, getHandle(), url, body,
                    LLVersionInfo::instance().getChannelAndVersion(), trimmed_summary));
}

// static
void LLFloaterReportBug::sendCoro(LLHandle<LLFloater> handle, std::string url, std::string body, std::string version,
                                  std::string summary)
{
    (void)version;   // the version header is added by LLReportTracker::postSigned
    const LLReportTracker::RelayReply reply = LLReportTracker::postSigned(url, body);
    const S32 code = reply.mStatus;
    const std::string& text = reply.mText;
    std::string issue_url, error, receipt;
    S32 issue_number = 0;
    boost::system::error_code ec;
    const boost::json::value doc = boost::json::parse(text, ec);
    if (!ec && doc.is_object())
    {
        const boost::json::object& obj = doc.as_object();
        if (const auto* v = obj.if_contains("url"); v && v->is_string()) issue_url = std::string(v->as_string());
        if (const auto* v = obj.if_contains("issue_number"); v && v->is_int64()) issue_number = (S32)v->as_int64();
        if (const auto* v = obj.if_contains("receipt"); v && v->is_string()) receipt = std::string(v->as_string());
        if (const auto* v = obj.if_contains("error"); v && v->is_string()) error = std::string(v->as_string());
    }
    if (error.empty() && !reply.mError.empty())
    {
        error = reply.mError;
    }
    const bool ok = code >= 200 && code < 300 && issue_number > 0 && !issue_url.empty();
    LL_INFOS("ReportBug") << "Bug report result: HTTP " << code << (ok ? llformat(", issue #%d", issue_number) : ", " + error)
                          << LL_ENDL;
    if (ok && !receipt.empty())
    {
        // Keep the receipt locally: it is what lets us follow up on / watch this report (#486 / #527).
        LLReportTracker::instance().addReport(issue_number, receipt, issue_url, summary);
    }

    if (LLFloaterReportBug* self = static_cast<LLFloaterReportBug*>(handle.get()))
    {
        self->onSendResult(ok, code, issue_url, issue_number, error);
    }
    else if (ok)
    {
        LLSD args;
        args["NUMBER"] = issue_number;
        args["URL"] = issue_url;
        LLNotificationsUtil::add("ReportBugFiled", args);
    }
}

// ---------------------------------------------------------------------------------
// MY REPORTS tab: follow-ups on reports filed earlier (#486), status shown from the relay (#527)

void LLFloaterReportBug::refreshMyReports()
{
    if (!mMyReports) return;
    const S32 keep = selectedReport();
    mMyReports->deleteAllItems();
    for (const LLFiledReport& r : LLReportTracker::instance().reports())
    {
        LLSD row;
        row["id"] = r.mNumber;
        row["columns"][0]["column"] = "number";
        row["columns"][0]["value"] = llformat("#%d", r.mNumber);
        row["columns"][1]["column"] = "summary";
        row["columns"][1]["value"] = r.mSummary;
        row["columns"][2]["column"] = "status";
        row["columns"][2]["value"] = LLReportTracker::statusText(r);
        mMyReports->addElement(row);
    }
    if (keep > 0) mMyReports->selectByID(LLSD(keep));
    updateFollowupControls();
}

S32 LLFloaterReportBug::selectedReport() const
{
    if (!mMyReports) return 0;
    LLScrollListItem* item = mMyReports->getFirstSelected();
    return item ? item->getValue().asInteger() : 0;
}

void LLFloaterReportBug::updateFollowupControls()
{
    const S32 number = selectedReport();
    mFollowSendBtn->setEnabled(number > 0 && !mFollowSending);
    mFollowText->setEnabled(!mFollowSending);
    mFollowOpenBtn->setEnabled(number > 0);
    mFollowForgetBtn->setEnabled(number > 0 && !mFollowSending);
    if (number > 0) LLReportTracker::instance().markSeen(number);
}

void LLFloaterReportBug::onFollowupOpen()
{
    LLFiledReport r;
    if (LLReportTracker::instance().find(selectedReport(), r) && r.mUrl.rfind("https://", 0) == 0)
    {
        LLWeb::loadURLExternal(r.mUrl);
    }
}

void LLFloaterReportBug::onFollowupForget()
{
    // Only removes the local record (and with it the ability to follow up from this viewer).
    LLReportTracker::instance().forget(selectedReport());
}

void LLFloaterReportBug::onFollowupSend()
{
    if (mFollowSending) return;
    LLFiledReport report;
    if (!LLReportTracker::instance().find(selectedReport(), report))
    {
        mStatus->setText(getString("followup_select"));
        return;
    }
    std::string comment = mFollowText->getText();
    LLStringUtil::trim(comment);
    if ((S32)utf8str_to_wstring(comment).size() < FOLLOWUP_MIN)
    {
        mStatus->setText(getString("followup_short"));
        return;
    }
    std::string avatar = mAvatar->getText();
    LLStringUtil::trim(avatar);
    LLSD payload = LLSD::emptyMap();
    payload["schema"] = 1;
    payload["issue_number"] = report.mNumber;
    payload["receipt"] = report.mReceipt;
    payload["comment"] = comment;
    payload["avatar"] = avatar;
    // Attachments follow the checkboxes on the REPORT tab (same scrubbed log / own-snapshot screenshot).
    if (mAttachLog->get() && !mLogExcerpt.empty()) payload["log"] = mLogExcerpt;
    if (mAttachShot->get() && !mShotJpeg.empty())
    {
        payload["screenshot"]["type"] = "image/jpeg";
        payload["screenshot"]["data"] = LLBase64::encode(mShotJpeg.data(), mShotJpeg.size());
    }
    const std::string body = valid_utf8(boost::json::serialize(LlsdToJson(payload)));
    const std::string url = LLReportTracker::relayEndpoint("followup");
    mFollowSending = true;
    mStatus->setText(getString("sending"));
    updateFollowupControls();
    LL_INFOS("ReportBug") << "Sending follow-up to #" << report.mNumber << " (" << body.size() << " bytes)" << LL_ENDL;
    LLCoros::instance().launch("LLFloaterReportBug::followupCoro",
        boost::bind(&LLFloaterReportBug::followupCoro, getHandle(), url, body, report.mNumber));
}

// static
void LLFloaterReportBug::followupCoro(LLHandle<LLFloater> handle, std::string url, std::string body, S32 number)
{
    const LLReportTracker::RelayReply reply = LLReportTracker::postSigned(url, body);
    std::string error;
    boost::system::error_code ec;
    const boost::json::value doc = boost::json::parse(reply.mText, ec);
    if (!ec && doc.is_object())
    {
        if (const auto* v = doc.as_object().if_contains("error"); v && v->is_string()) error = std::string(v->as_string());
    }
    if (error.empty()) error = reply.mError;
    const bool ok = reply.mStatus >= 200 && reply.mStatus < 300;
    LL_INFOS("ReportBug") << "Follow-up to #" << number << ": HTTP " << reply.mStatus << (ok ? "" : ", " + error) << LL_ENDL;
    if (ok) LLReportTracker::instance().pollSoon();
    if (LLFloaterReportBug* self = static_cast<LLFloaterReportBug*>(handle.get()))
    {
        self->onFollowupResult(ok, reply.mStatus, error);
    }
}

void LLFloaterReportBug::onFollowupResult(bool ok, S32 status, const std::string& error)
{
    mFollowSending = false;
    if (ok)
    {
        mFollowText->clear();
        mStatus->setText(getString("followup_sent"));
    }
    else
    {
        LLStringUtil::format_map_t args;
        args["[STATUS]"] = status > 0 ? llformat("%d", status) : std::string("-");
        args["[ERROR]"] = error.empty() ? std::string("unknown error") : clip(error, 200);
        mStatus->setText(getString(status == 429 ? "error_rate" : "error_send", args));
    }
    updateFollowupControls();
}

void LLFloaterReportBug::onSendResult(bool ok, S32 status, const std::string& issue_url, S32 issue_number,
                                      const std::string& error)
{
    mSending = false;
    if (ok)
    {
        mSent = true;
        LLStringUtil::format_map_t args;
        args["[NUMBER]"] = llformat("%d", issue_number);
        args["[URL]"] = issue_url;
        mStatus->setText(getString("filed", args));
        if (!mCrashEvent.empty()) mCrashEvent.clear();
    }
    else
    {
        LLStringUtil::format_map_t args;
        args["[STATUS]"] = status > 0 ? llformat("%d", status) : std::string("-");
        args["[ERROR]"] = error.empty() ? std::string("unknown error") : clip(error, 200);
        mStatus->setText(getString(status == 429 ? "error_rate" : "error_send", args));
    }
    updateControls();
}

// ---------------------------------------------------------------------------------

// static
void LLFloaterReportBug::showForCrash()
{
    LLFloaterReportBug* floater = LLFloaterReg::showTypedInstance<LLFloaterReportBug>("report_bug");
    if (floater && !floater->mSent && !floater->mSending)
    {
        floater->setCrashMode();
    }
}

// static
void LLFloaterReportBug::offerCrashReportIfNeeded()
{
    if (sCrashOffered) return;
    sCrashOffered = true;
    const std::string what = exec_event_name(gLastExecEvent);
    if (what.empty()) return;
    // CrashSubmitBehavior 2 = never; never auto-send whatever its value.
    if (!gSavedSettings.getBOOL("ReportBugCrashPrompt") || gCrashSettings.getS32("CrashSubmitBehavior") == 2)
    {
        return;
    }
    LLSD args;
    args["WHAT"] = what;
    LLNotificationsUtil::add("ReportBugCrashPrompt", args, LLSD(), [](const LLSD& notification, const LLSD& response)
    {
        switch (LLNotificationsUtil::getSelectedOption(notification, response))
        {
            case 0: showForCrash(); break;
            case 2: gSavedSettings.setBOOL("ReportBugCrashPrompt", false); break;
            default: break;
        }
        return false;
    });
}
