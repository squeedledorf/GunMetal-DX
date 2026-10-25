/**
 * @file sldatausage.cpp
 * @brief Per-session network data accounting by category (#529).
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

#include "sldatausage.h"

#include <algorithm>
#include <atomic>
#include <sstream>
#include <vector>

#include "httprequest.h"
#include "llappcorehttp.h"
#include "llappviewer.h"
#include "llframetimer.h"
#include "llgputexcache.h"
#include "llmessagetemplate.h"
#include "llviewermedia.h"
#include "llvoiceclient.h"
#include "message.h"
#include "llavatarnamecache.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include <unordered_map>

namespace
{
    constexpr F64 MB = 1024.0 * 1024.0;
    constexpr F64 LOG_INTERVAL = 30.0 * 60.0;   // seconds between periodic summaries
    constexpr size_t REPORT_VALUE_MAX = 200;    // bug report relay per-field limit
    constexpr U32 UDP_HEADER_BYTES = 28;        // IPv4 + UDP header per datagram (estimate only)

    // Texture fetch threads.
    std::atomic<U64> sTexCacheHits{ 0 };
    std::atomic<U64> sTexCacheMisses{ 0 };

    // Main thread (atomics anyway: negligible cost, no ordering worries).
    std::atomic<U64> sObjCacheHits{ 0 };
    std::atomic<U64> sObjCacheMisses{ 0 };
    std::atomic<U64> sObjCacheCrcMisses{ 0 };
    std::atomic<U32> sRegionCacheLoads{ 0 };
    std::atomic<U32> sRegionCacheUnusable{ 0 };
    std::atomic<U64> sRegionCacheEntries{ 0 };

    // Activity sampled once a second (main thread only).
    U64 sMediaSeconds = 0;          // seconds with at least one media instance playing
    U64 sMediaInstanceSeconds = 0;  // sum over seconds of loaded media instances
    U32 sMediaPeakLoaded = 0;       // most media instances (plugin processes) loaded at once
    U64 sMusicSeconds = 0;          // parcel audio stream playing
    U64 sVoiceSeconds = 0;          // connected to a voice channel

    struct HttpCategory
    {
        const char* mName;
        LLCore::HttpRequest::TransferStats mStats;
    };

    const char* http_category_name(LLAppCoreHttp::EAppPolicy policy)
    {
        switch (policy)
        {
            case LLAppCoreHttp::AP_DEFAULT:     return "other";     // caps, login, update check, ...
            case LLAppCoreHttp::AP_TEXTURE:     return "texture";
            case LLAppCoreHttp::AP_MESH1:       return "mesh";
            case LLAppCoreHttp::AP_MESH2:       return "mesh2";
            case LLAppCoreHttp::AP_LARGE_MESH:  return "large mesh";
            case LLAppCoreHttp::AP_UPLOADS:     return "upload";
            case LLAppCoreHttp::AP_LONG_POLL:   return "event poll";
            case LLAppCoreHttp::AP_INVENTORY:   return "inventory";  // AIS + reporting
            case LLAppCoreHttp::AP_MATERIALS:   return "materials";
            case LLAppCoreHttp::AP_AGENT:       return "agent";
            default:                            return "?";
        }
    }

    // One entry per distinct policy class (AP_REPORTING aliases AP_INVENTORY, and a class
    // that failed to be created falls back to the default one), sorted by bytes received.
    std::vector<HttpCategory> http_categories(LLCore::HttpRequest::TransferStats& total)
    {
        std::vector<HttpCategory> out;
        std::vector<LLCore::HttpRequest::policy_t> seen;
        total = LLCore::HttpRequest::TransferStats();
        LLAppCoreHttp& ach = LLAppViewer::instance()->getAppCoreHttp();
        for (int i = 0; i < LLAppCoreHttp::AP_COUNT; ++i)
        {
            const LLAppCoreHttp::EAppPolicy app_policy = static_cast<LLAppCoreHttp::EAppPolicy>(i);
            const LLCore::HttpRequest::policy_t policy = ach.getPolicy(app_policy);
            if (std::find(seen.begin(), seen.end(), policy) != seen.end()) continue;
            seen.push_back(policy);
            HttpCategory cat;
            cat.mName = http_category_name(app_policy);
            LLCore::HttpRequest::getTransferStats(policy, cat.mStats);
            total.mRequests += cat.mStats.mRequests;
            total.mFailed += cat.mStats.mFailed;
            total.mRetries += cat.mStats.mRetries;
            total.mBytesIn += cat.mStats.mBytesIn;
            total.mBytesOut += cat.mStats.mBytesOut;
            if (cat.mStats.mRequests || cat.mStats.mBytesIn || cat.mStats.mBytesOut) out.push_back(cat);
        }
        std::stable_sort(out.begin(), out.end(), [](const HttpCategory& a, const HttpCategory& b)
                         { return a.mStats.mBytesIn > b.mStats.mBytesIn; });
        return out;
    }

    struct UdpTotals
    {
        U64 mBytesIn = 0, mBytesOut = 0, mPacketsIn = 0, mPacketsOut = 0;
    };

    UdpTotals udp_totals()
    {
        UdpTotals t;
        if (gMessageSystem)
        {
            t.mBytesIn = gMessageSystem->mPacketRing.getSessionBytesIn();
            t.mBytesOut = gMessageSystem->mPacketRing.getSessionBytesOut();
            t.mPacketsIn = gMessageSystem->mPacketRing.getSessionPacketsIn();
            t.mPacketsOut = gMessageSystem->mPacketRing.getSessionPacketsOut();
        }
        return t;
    }

    // Received messages by wire bytes, largest first.
    std::vector<const LLMessageTemplate*> udp_top_messages(size_t count, U64* counted_bytes = nullptr)
    {
        std::vector<const LLMessageTemplate*> all;
        U64 sum = 0;
        if (gMessageSystem)
        {
            for (const auto& kv : gMessageSystem->getMessageTemplates())
            {
                const LLMessageTemplate* t = kv.second;
                if (t && t->mSessionReceiveBytes)
                {
                    all.push_back(t);
                    sum += t->mSessionReceiveBytes;
                }
            }
        }
        std::sort(all.begin(), all.end(), [](const LLMessageTemplate* a, const LLMessageTemplate* b)
                  { return a->mSessionReceiveBytes > b->mSessionReceiveBytes; });
        if (all.size() > count) all.resize(count);
        if (counted_bytes) *counted_bytes = sum;
        return all;
    }

    F64 session_seconds()
    {
        return llmax((F64)LLFrameTimer::getElapsedSeconds(), 1.0);
    }

    std::string duration_text(U64 seconds)
    {
        const U64 h = seconds / 3600, m = (seconds / 60) % 60;
        if (h) return llformat("%lluh%02llum", (unsigned long long)h, (unsigned long long)m);
        return llformat("%llum%02llus", (unsigned long long)m, (unsigned long long)(seconds % 60));
    }

    std::string percent_text(U64 hits, U64 misses)
    {
        const U64 n = hits + misses;
        if (!n) return "n/a";
        return llformat("%.0f%%", 100.0 * (F64)hits / (F64)n);
    }

    std::string mb(U64 bytes)
    {
        return llformat("%.1f MB", (F64)bytes / MB);
    }

    std::string clip(const std::string& s)
    {
        if (s.size() <= REPORT_VALUE_MAX) return s;
        return s.substr(0, REPORT_VALUE_MAX - 3) + "...";
    }

    // Appends "piece" (with separator) while the result stays within the relay limit.
    void append_bounded(std::string& out, const std::string& piece, const char* sep = ", ")
    {
        const std::string candidate = out.empty() ? piece : out + sep + piece;
        if (candidate.size() <= REPORT_VALUE_MAX) out = candidate;
    }

    std::string media_text()
    {
        return llformat("media playing %s (peak %u loaded), music stream %s, voice connected %s; bytes not measured",
                        duration_text(sMediaSeconds).c_str(), sMediaPeakLoaded,
                        duration_text(sMusicSeconds).c_str(), duration_text(sVoiceSeconds).c_str());
    }

    std::string cache_text()
    {
        std::string out = llformat("textures disk cache %s (%llu hit / %llu miss)",
                                   percent_text(sTexCacheHits, sTexCacheMisses).c_str(),
                                   (unsigned long long)sTexCacheHits.load(), (unsigned long long)sTexCacheMisses.load());
        if (LLGpuTexCache::isEnabled())
        {
            const LLGpuTexCache::Stats g = LLGpuTexCache::getStats();
            out += llformat(", GPU-ready %s (%llu / %llu)", percent_text(g.hits, g.misses).c_str(),
                            (unsigned long long)g.hits, (unsigned long long)g.misses);
        }
        out += llformat("; objects %s (%llu hit / %llu miss, %llu stale CRC); regions %u (%u without usable cache, %llu objects loaded)",
                        percent_text(sObjCacheHits, sObjCacheMisses).c_str(),
                        (unsigned long long)sObjCacheHits.load(), (unsigned long long)sObjCacheMisses.load(),
                        (unsigned long long)sObjCacheCrcMisses.load(), sRegionCacheLoads.load(),
                        sRegionCacheUnusable.load(), (unsigned long long)sRegionCacheEntries.load());
        return out;
    }

    void sample_activity()
    {
        if (LLViewerMedia::instanceExists())
        {
            LLViewerMedia* media = LLViewerMedia::getInstance();
            U32 loaded = 0, playing = 0;
            for (LLViewerMediaImpl* impl : media->getPriorityList())
            {
                if (!impl || !impl->hasMedia()) continue;
                ++loaded;
                if (impl->isMediaPlaying()) ++playing;
            }
            sMediaInstanceSeconds += loaded;
            sMediaPeakLoaded = llmax(sMediaPeakLoaded, loaded);
            if (playing) ++sMediaSeconds;
            if (media->isParcelAudioPlaying()) ++sMusicSeconds;
        }
        if (LLVoiceClient::instanceExists() && LLVoiceClient::getInstance()->isVoiceWorking())
        {
            ++sVoiceSeconds;
        }
    }
}

namespace SLDataUsage
{

void noteTextureCache(bool hit)
{
    (hit ? sTexCacheHits : sTexCacheMisses).fetch_add(1, std::memory_order_relaxed);
}

void noteObjectCacheProbe(bool hit, bool crc_miss)
{
    if (hit)
    {
        sObjCacheHits.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    sObjCacheMisses.fetch_add(1, std::memory_order_relaxed);
    if (crc_miss) sObjCacheCrcMisses.fetch_add(1, std::memory_order_relaxed);
}

void noteRegionCacheLoad(bool usable, U32 entries)
{
    sRegionCacheLoads.fetch_add(1, std::memory_order_relaxed);
    if (!usable) sRegionCacheUnusable.fetch_add(1, std::memory_order_relaxed);
    sRegionCacheEntries.fetch_add(entries, std::memory_order_relaxed);
}

static void logTopObjects(const std::string& when);

void idleTick()
{
    static F64 s_next_sample = 0.0;
    static F64 s_next_log = LOG_INTERVAL;
    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now < s_next_sample) return;
    s_next_sample = now + 1.0;
    sample_activity();
    static bool s_early_top_logged = false;
    if (!s_early_top_logged && now >= 300.0)
    {
        // Name the busiest objects after 5 minutes instead of waiting for the first 30-minute summary.
        s_early_top_logged = true;
        logTopObjects("5-minute");
    }
    if (now >= s_next_log)
    {
        s_next_log = now + LOG_INTERVAL;
        logSummary("periodic");
    }
}

// ---- per-object update counts (main thread), reset after each summary ----
namespace
{
// What a full update changed, compared with the object's previous full update
enum EUpdateChange { CH_POS, CH_ROT, CH_SCALE, CH_TEXT, CH_COLOR, CH_TEXTURE, CH_FACE, CH_FLAGS, CH_NONE, CH_COUNT };
const char* const UPDATE_CHANGE_NAMES[CH_COUNT] = { "pos", "rot", "scale", "text", "color", "texture", "face-params", "flags", "nothing" };

struct ObjectSignature
{
    U64 mPos = 0, mRot = 0, mScale = 0, mText = 0, mColor = 0, mTexture = 0, mFace = 0, mFlags = 0;
};

struct ObjectUpdateCount
{
    U32 mFull = 0;      // OUT_FULL + OUT_FULL_COMPRESSED
    U32 mTerse = 0;     // OUT_TERSE_IMPROVED
    U32 mChanged[CH_COUNT] = {};
    bool mHaveSig = false;
    ObjectSignature mSig;
};

U64 hash_bytes(const void* data, size_t size, U64 h = 1469598103934665603ULL)
{
    const U8* p = (const U8*)data;
    for (size_t i = 0; i < size; ++i)
    {
        h = (h ^ p[i]) * 1099511628211ULL;
    }
    return h;
}

ObjectSignature signature_of(const LLViewerObject* object)
{
    ObjectSignature sig;
    const LLVector3 pos = object->getPositionRegion();
    const LLQuaternion rot = object->getRotationRegion();
    const LLVector3 scale = object->getScale();
    sig.mPos = hash_bytes(pos.mV, sizeof(pos.mV));
    sig.mRot = hash_bytes(rot.mQ, sizeof(rot.mQ));
    sig.mScale = hash_bytes(scale.mV, sizeof(scale.mV));
    sig.mText = hash_bytes(object->mHudText.data(), object->mHudText.size());
    const U32 flags = (object->flagScripted() << 0) | (object->flagHandleTouch() << 1) | (object->flagPhantom() << 2) |
                      (object->flagTemporaryOnRez() << 3) | (object->flagVolumeDetect() << 4) | (object->flagInventoryEmpty() << 5) |
                      (object->flagTakesMoney() << 6) | (object->flagAnimSource() << 7) | (object->flagCameraSource() << 8);
    sig.mFlags = hash_bytes(&flags, sizeof(flags));
    U64 color = 1469598103934665603ULL, texture = color, face = color;
    for (U8 i = 0; i < object->getNumTEs(); ++i)
    {
        const LLTextureEntry* te = object->getTE(i);
        if (!te)
        {
            continue;
        }
        color = hash_bytes(te->getColor().mV, sizeof(te->getColor().mV), color);
        texture = hash_bytes(te->getID().mData, sizeof(te->getID().mData), texture);
        F32 params[6] = { te->getGlow(), te->getRotation(), 0.f, 0.f, 0.f, 0.f };
        te->getScale(&params[2], &params[3]);
        te->getOffset(&params[4], &params[5]);
        const U8 bits[3] = { te->getBumpShinyFullbright(), te->getMediaTexGen(), (U8)te->getMaterialID().isNull() };
        face = hash_bytes(params, sizeof(params), face);
        face = hash_bytes(bits, sizeof(bits), face);
    }
    sig.mColor = color;
    sig.mTexture = texture;
    sig.mFace = face;
    return sig;
}

std::string changes_text(const ObjectUpdateCount& count)
{
    std::string text;
    for (S32 c = 0; c < CH_COUNT; ++c)
    {
        if (count.mChanged[c])
        {
            text += llformat("%s%s %u", text.empty() ? "" : ", ", UPDATE_CHANGE_NAMES[c], count.mChanged[c]);
        }
    }
    return text.empty() ? std::string("first update only") : text;
}
constexpr size_t MAX_COUNTED_OBJECTS = 8192;   // further objects only add to the "others" total
std::unordered_map<LLUUID, ObjectUpdateCount> sObjectUpdates;
U32 sObjectUpdatesUncounted = 0;

std::string describe_object(const LLUUID& id)
{
    LLViewerObject* object = gObjectList.findObject(id);
    if (!object)
    {
        return id.asString() + " (gone)";
    }
    auto name_of = [](const LLUUID& av_id)
    {
        LLAvatarName av_name;
        return LLAvatarNameCache::get(av_id, &av_name) ? av_name.getCompleteName() : av_id.asString();
    };
    std::string where = object->getRegion() ? object->getRegion()->getName() : std::string("?");
    if (object->isAvatar())
    {
        return "avatar " + name_of(id) + " [" + where + "]";
    }
    if (LLVOAvatar* wearer = object->getAvatarAncestor())
    {
        return llformat("attachment %s on %s [%s]", id.asString().c_str(), name_of(wearer->getID()).c_str(), where.c_str());
    }
    LLViewerObject* root = object->getRootEdit();
    const LLVector3 pos = object->getPositionRegion();
    const LLVector3 scale = object->getScale();
    std::string text = object->mHudText.substr(0, 40);
    LLStringUtil::replaceChar(text, '\n', ' ');
    return llformat("object %s (root %s, pcode %d, owner %s, at <%.0f, %.0f, %.0f> size <%.2f, %.2f, %.2f>%s) [%s]",
                    id.asString().c_str(), root ? root->getID().asString().c_str() : "?", (S32)object->getPCode(),
                    object->mOwnerID.isNull() ? "?" : name_of(object->mOwnerID).c_str(), pos.mV[VX], pos.mV[VY], pos.mV[VZ],
                    scale.mV[VX], scale.mV[VY], scale.mV[VZ],
                    text.empty() ? "" : (", text \"" + text + "\"").c_str(), where.c_str());
}

std::string top_updates_text(bool full)
{
    std::vector<std::pair<U32, LLUUID>> ranked;
    ranked.reserve(sObjectUpdates.size());
    U64 total = 0;
    for (const auto& [id, count] : sObjectUpdates)
    {
        const U32 n = full ? count.mFull : count.mTerse;
        total += n;
        if (n)
        {
            ranked.emplace_back(n, id);
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::ostringstream text;
    text << (full ? "top full object updates" : "top terse (movement) updates") << " this period (" << total << " from "
         << ranked.size() << " objects):";
    const size_t shown = std::min<size_t>(ranked.size(), full ? 10 : 5);
    for (size_t i = 0; i < shown; ++i)
    {
        text << " " << ranked[i].first << " " << describe_object(ranked[i].second);
        if (full)
        {
            text << " {changed: " << changes_text(sObjectUpdates[ranked[i].second]) << "}";
        }
        text << ";";
    }
    return text.str();
}
}

void noteObjectUpdate(const LLViewerObject* object, S32 update_type)
{
    if (!object)
    {
        return;
    }
    auto it = sObjectUpdates.find(object->getID());
    if (it == sObjectUpdates.end())
    {
        if (sObjectUpdates.size() >= MAX_COUNTED_OBJECTS)
        {
            ++sObjectUpdatesUncounted;
            return;
        }
        it = sObjectUpdates.emplace(object->getID(), ObjectUpdateCount()).first;
    }
    if (update_type == OUT_TERSE_IMPROVED)
    {
        ++it->second.mTerse;
    }
    else if (update_type == OUT_FULL || update_type == OUT_FULL_COMPRESSED)
    {
        ObjectUpdateCount& count = it->second;
        ++count.mFull;
        const ObjectSignature sig = signature_of(object);
        if (count.mHaveSig)
        {
            const U64 before[CH_NONE] = { count.mSig.mPos, count.mSig.mRot, count.mSig.mScale, count.mSig.mText,
                                          count.mSig.mColor, count.mSig.mTexture, count.mSig.mFace, count.mSig.mFlags };
            const U64 after[CH_NONE] = { sig.mPos, sig.mRot, sig.mScale, sig.mText, sig.mColor, sig.mTexture, sig.mFace, sig.mFlags };
            bool any = false;
            for (S32 c = 0; c < CH_NONE; ++c)
            {
                if (before[c] != after[c])
                {
                    ++count.mChanged[c];
                    any = true;
                }
            }
            if (!any)
            {
                ++count.mChanged[CH_NONE];
            }
        }
        count.mSig = sig;
        count.mHaveSig = true;
    }
}

static void logTopObjects(const std::string& when)
{
    // Counts keep accumulating; the 30-minute summary still covers its whole period.
    LL_INFOS("DataUsage") << "DataUsage: [" << when << "] " << top_updates_text(true) << LL_ENDL;
    LL_INFOS("DataUsage") << "DataUsage: [" << when << "] " << top_updates_text(false) << LL_ENDL;
}

void logSummary(const std::string& when)
{
    const F64 secs = session_seconds();
    LLCore::HttpRequest::TransferStats http;
    const std::vector<HttpCategory> cats = http_categories(http);
    const UdpTotals udp = udp_totals();
    const U64 in = http.mBytesIn + udp.mBytesIn, out = http.mBytesOut + udp.mBytesOut;

    LL_INFOS("DataUsage") << "DataUsage: [" << when << "] session " << duration_text((U64)secs) << ": received "
                          << mb(in) << " (HTTP " << mb(http.mBytesIn) << " + UDP " << mb(udp.mBytesIn) << "), sent "
                          << mb(out) << " (HTTP " << mb(http.mBytesOut) << " + UDP " << mb(udp.mBytesOut) << "); average "
                          << llformat("%.3f", (F64)in * 8.0 / secs / 1.0e6) << " Mbit/s down, "
                          << llformat("%.3f", (F64)out * 8.0 / secs / 1.0e6) << " Mbit/s up; "
                          << "with IP/UDP headers ~" << mb(in + udp.mPacketsIn * UDP_HEADER_BYTES) << " in" << LL_ENDL;

    std::ostringstream h;
    h << "DataUsage:   HTTP " << http.mRequests << " requests (" << http.mFailed << " failed, " << http.mRetries << " retries):";
    for (const HttpCategory& c : cats)
    {
        h << " " << c.mName << " " << mb(c.mStats.mBytesIn) << " in / " << mb(c.mStats.mBytesOut) << " out, "
          << c.mStats.mRequests << " req";
        if (c.mStats.mFailed || c.mStats.mRetries)
        {
            h << " (" << c.mStats.mFailed << " failed, " << c.mStats.mRetries << " retries)";
        }
        h << ";";
    }
    LL_INFOS("DataUsage") << h.str() << LL_ENDL;

    U64 counted = 0;
    const std::vector<const LLMessageTemplate*> top = udp_top_messages(10, &counted);
    std::ostringstream u;
    u << "DataUsage:   UDP " << udp.mPacketsIn << " packets in / " << udp.mPacketsOut << " out; top received:";
    for (const LLMessageTemplate* t : top)
    {
        u << " " << t->mName << " " << mb(t->mSessionReceiveBytes) << " (" << t->mSessionReceiveCount << ");";
    }
    if (udp.mBytesIn > counted)
    {
        u << " invalid/duplicate/off-circuit " << mb(udp.mBytesIn - counted) << ";";
    }
    LL_INFOS("DataUsage") << u.str() << LL_ENDL;

    LL_INFOS("DataUsage") << "DataUsage:   cache: " << cache_text() << LL_ENDL;
    LL_INFOS("DataUsage") << "DataUsage:   " << media_text()
                          << llformat(" (avg %.1f media instances loaded)", (F64)sMediaInstanceSeconds / secs)
                          << " -- media/music run in SLPlugin processes, voice bytes are not reported by WebRTC" << LL_ENDL;
    LL_INFOS("DataUsage") << "DataUsage:   " << top_updates_text(true) << LL_ENDL;
    LL_INFOS("DataUsage") << "DataUsage:   " << top_updates_text(false)
                          << (sObjectUpdatesUncounted ? llformat(" (+%u updates from objects past the %u-object cap)", sObjectUpdatesUncounted, (U32)MAX_COUNTED_OBJECTS) : std::string()) << LL_ENDL;
    sObjectUpdates.clear();
    sObjectUpdatesUncounted = 0;
}

std::string aboutText()
{
    LLCore::HttpRequest::TransferStats http;
    http_categories(http);
    const UdpTotals udp = udp_totals();
    return llformat("Downloaded this session: %s (HTTP %s, UDP %s); sent %s. Media, music and voice not included.",
                    mb(http.mBytesIn + udp.mBytesIn).c_str(), mb(http.mBytesIn).c_str(), mb(udp.mBytesIn).c_str(),
                    mb(http.mBytesOut + udp.mBytesOut).c_str());
}

std::string reportText(int part)
{
    const F64 secs = session_seconds();
    switch (part)
    {
        case 0:
        {
            LLCore::HttpRequest::TransferStats http;
            http_categories(http);
            const UdpTotals udp = udp_totals();
            const U64 in = http.mBytesIn + udp.mBytesIn;
            return clip(llformat("%s in / %s out over %s (%.3f Mbit/s avg down); HTTP %s in, %llu req, %llu failed, %llu retries; UDP %s in",
                                 mb(in).c_str(), mb(http.mBytesOut + udp.mBytesOut).c_str(), duration_text((U64)secs).c_str(),
                                 (F64)in * 8.0 / secs / 1.0e6, mb(http.mBytesIn).c_str(),
                                 (unsigned long long)http.mRequests, (unsigned long long)http.mFailed,
                                 (unsigned long long)http.mRetries, mb(udp.mBytesIn).c_str()));
        }
        case 1:
        {
            LLCore::HttpRequest::TransferStats http;
            std::string out;
            for (const HttpCategory& c : http_categories(http))
            {
                append_bounded(out, llformat("%s %.1fMB/%llu", c.mName, (F64)c.mStats.mBytesIn / MB,
                                             (unsigned long long)c.mStats.mRequests));
            }
            return out;
        }
        case 2:
        {
            std::string out;
            for (const LLMessageTemplate* t : udp_top_messages(10))
            {
                append_bounded(out, llformat("%s %.1fMB", t->mName, (F64)t->mSessionReceiveBytes / MB));
            }
            return out;
        }
        case 3:
        {
            std::string out = llformat("tex disk %s, obj %s (%llu CRC), regions %u/%u no cache",
                                       percent_text(sTexCacheHits, sTexCacheMisses).c_str(),
                                       percent_text(sObjCacheHits, sObjCacheMisses).c_str(),
                                       (unsigned long long)sObjCacheCrcMisses.load(),
                                       sRegionCacheUnusable.load(), sRegionCacheLoads.load());
            append_bounded(out, llformat("media %s, music %s, voice %s (unmeasured)", duration_text(sMediaSeconds).c_str(),
                                         duration_text(sMusicSeconds).c_str(), duration_text(sVoiceSeconds).c_str()), "; ");
            return clip(out);
        }
        default:
            return std::string();
    }
}

} // namespace SLDataUsage
