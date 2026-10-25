/**
 * @file llchatimagecache.cpp
 * @brief Chat inline image previews (#265): URL policy, fetch, decode and the
 *        session thumbnail cache.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Linden Research, Inc.
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

#include "llchatimagecache.h"

#include "llapp.h"
#include "llchatimagegif.h"
#include "llchatimagewebp.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "httpaddresspolicy.h"
#include "llhttpconstants.h"
#include "llimagejpeg.h"
#include "llimagepng.h"
#include "llviewercontrol.h"
#include "llviewermedia.h"
#include "llviewertexture.h"
#include "workqueue.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
    const char* const LOG_TAG = "ChatImage";

    bool ends_with(const std::string& s, const std::string& suffix)
    {
        return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
    }

    bool starts_with(const std::string& s, const std::string& prefix)
    {
        return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
    }

    // WHATWG "ends in a number": the last label is all digits or 0x-hex. Such a host
    // is an IPv4 literal to the URL parser (and to libcurl), in any of its odd forms
    // (2130706433, 0x7f.1, 0177.0.0.1, ...).
    bool is_numeric_host(const std::string& host)
    {
        const size_t dot = host.rfind('.');
        const std::string last = (dot == std::string::npos) ? host : host.substr(dot + 1);
        if (last.empty())
        {
            return false;
        }
        if (starts_with(last, "0x"))
        {
            return true;
        }
        return std::all_of(last.begin(), last.end(), [](char c) { return c >= '0' && c <= '9'; });
    }

    // Strict dotted quad (four decimal parts 0-255, no leading zeros). Anything else
    // that is numeric is refused by the caller rather than interpreted.
    bool parse_dotted_quad(const std::string& host, U32& out)
    {
        U32 value = 0;
        S32 parts = 0;
        size_t i = 0;
        while (i <= host.size())
        {
            const size_t dot = host.find('.', i);
            const size_t end = (dot == std::string::npos) ? host.size() : dot;
            const std::string part = host.substr(i, end - i);
            if (part.empty() || part.size() > 3 || (part.size() > 1 && part[0] == '0'))
            {
                return false;
            }
            U32 n = 0;
            for (char c : part)
            {
                if (c < '0' || c > '9') return false;
                n = n * 10 + U32(c - '0');
            }
            if (n > 255 || ++parts > 4)
            {
                return false;
            }
            value = (value << 8) | n;
            if (dot == std::string::npos)
            {
                break;
            }
            i = dot + 1;
        }
        if (parts != 4)
        {
            return false;
        }
        out = value;
        return true;
    }

    U32 be32(const U8* p)
    {
        return (U32(p[0]) << 24) | (U32(p[1]) << 16) | (U32(p[2]) << 8) | U32(p[3]);
    }

    enum EMagic { MAGIC_UNKNOWN, MAGIC_PNG, MAGIC_JPEG, MAGIC_GIF, MAGIC_WEBP };

    EMagic sniff(const std::vector<U8>& d)
    {
        static const U8 png_sig[8] = { 0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A };
        if (d.size() >= 8 && memcmp(d.data(), png_sig, 8) == 0) return MAGIC_PNG;
        if (d.size() >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return MAGIC_JPEG;
        if (d.size() >= 6 && (memcmp(d.data(), "GIF87a", 6) == 0 || memcmp(d.data(), "GIF89a", 6) == 0)) return MAGIC_GIF;
        if (d.size() >= 12 && memcmp(d.data(), "RIFF", 4) == 0 && memcmp(d.data() + 8, "WEBP", 4) == 0) return MAGIC_WEBP;
        return MAGIC_UNKNOWN;
    }

    // Width/height from the PNG IHDR chunk (LLPngWrapper truncates to U16, so the
    // size limit is checked here, before the decoder sees the data).
    bool png_dims(const std::vector<U8>& d, U32& w, U32& h)
    {
        if (d.size() < 24 || memcmp(d.data() + 12, "IHDR", 4) != 0)
        {
            return false;
        }
        w = be32(d.data() + 16);
        h = be32(d.data() + 20);
        return true;
    }

    // Width/height from the first JPEG SOFn segment (bounded marker walk).
    bool jpeg_dims(const std::vector<U8>& d, U32& w, U32& h)
    {
        const size_t n = d.size();
        size_t i = 2;   // after SOI
        S32 guard = 0;
        while (i + 4 <= n && ++guard < 4096)
        {
            if (d[i] != 0xFF)
            {
                return false;
            }
            const U8 m = d[i + 1];
            if (m == 0xFF)
            {
                ++i;            // fill byte
                continue;
            }
            i += 2;
            if (m == 0x01 || (m >= 0xD0 && m <= 0xD8))
            {
                continue;       // TEM, RSTn, SOI: no length
            }
            if (m == 0xD9 || m == 0xDA)
            {
                return false;   // EOI / SOS before any SOF
            }
            if (i + 2 > n)
            {
                return false;
            }
            const size_t len = (size_t(d[i]) << 8) | size_t(d[i + 1]);
            if (len < 2 || i + len > n)
            {
                return false;
            }
            if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC)
            {
                if (len < 8)
                {
                    return false;
                }
                h = (U32(d[i + 3]) << 8) | U32(d[i + 4]);
                w = (U32(d[i + 5]) << 8) | U32(d[i + 6]);
                return true;
            }
            i += len;
        }
        return false;
    }

    // "image/png; charset=x" -> "image/png"
    std::string normalize_mime(const std::string& raw)
    {
        std::string mime = raw.substr(0, raw.find(';'));
        LLStringUtil::trim(mime);
        LLStringUtil::toLower(mime);
        return mime;
    }

    bool is_png_mime(const std::string& m)  { return m == "image/png" || m == "image/x-png"; }
    bool is_jpeg_mime(const std::string& m) { return m == "image/jpeg" || m == "image/jpg" || m == "image/pjpeg"; }
    bool is_gif_mime(const std::string& m)  { return m == "image/gif"; }
    bool is_webp_mime(const std::string& m) { return m == "image/webp"; }

    // Canonical mime of an accepted Content-Type (empty when not accepted).
    std::string canonical_mime(const std::string& m)
    {
        if (is_png_mime(m))  return "image/png";
        if (is_jpeg_mime(m)) return "image/jpeg";
        if (is_gif_mime(m))  return "image/gif";
        if (is_webp_mime(m)) return "image/webp";
        return std::string();
    }

    // Resolve a Location header against the URL that returned it.
    std::string resolve_location(const std::string& base, const std::string& loc)
    {
        std::string lower = loc.substr(0, 8);
        LLStringUtil::toLower(lower);
        if (starts_with(lower, "http://") || starts_with(lower, "https://"))
        {
            return loc;
        }
        const size_t scheme_end = base.find("://");
        if (scheme_end == std::string::npos)
        {
            return std::string();
        }
        if (starts_with(loc, "//"))
        {
            return base.substr(0, scheme_end + 1) + loc;
        }
        const size_t auth_end = base.find_first_of("/?#", scheme_end + 3);
        const std::string origin = base.substr(0, auth_end);
        if (starts_with(loc, "/"))
        {
            return origin + loc;
        }
        if (loc.find(':') != std::string::npos && loc.find(':') < loc.find_first_of("/?#"))
        {
            return std::string();   // some other scheme: refused by checkUrl anyway
        }
        // Relative to the base path's directory.
        std::string path = (auth_end == std::string::npos) ? std::string("/") : base.substr(auth_end);
        path = path.substr(0, path.find_first_of("?#"));
        const size_t slash = path.rfind('/');
        path = (slash == std::string::npos) ? std::string("/") : path.substr(0, slash + 1);
        return origin + path + loc;
    }

    const std::string& user_agent()
    {
        // The viewer's ordinary web user agent; computed once (the getter logs).
        static const std::string ua = LLViewerMedia::getInstance()->getCurrentUserAgent();
        return ua;
    }
}

void LLChatImageAnim::updateDuration()
{
    duration = 0.0;
    for (F32 d : delays)
    {
        duration += (F64)d;
    }
}

void LLChatImageAnim::clear()
{
    frames.clear();
    delays.clear();
    duration = 0.0;
    loops = 0;
}

size_t LLChatImageAnim::frameAt(F64 play_time, bool& finished) const
{
    finished = false;
    if (!isAnimated())
    {
        finished = true;
        return 0;
    }
    F64 t = llmax(0.0, play_time);
    if (loops > 0 && t >= duration * (F64)loops)
    {
        finished = true;
        return frames.size() - 1;
    }
    t = fmod(t, duration);
    for (size_t i = 0; i < delays.size(); ++i)
    {
        if (t < (F64)delays[i])
        {
            return i;
        }
        t -= (F64)delays[i];
    }
    return frames.size() - 1;   // rounding at the very end of a loop
}

LLChatImageCache::LLChatImageCache()
{
    LLControlVariablePtr budget = gSavedSettings.getControl("ChatImageCacheMB");
    if (budget)
    {
        mBudgetConnection = budget->getSignal()->connect(
            [this](LLControlVariable*, const LLSD&, const LLSD&)
            {
                enforceBudget(std::string());
                logStats("budget changed");
            });
    }
}

LLChatImageCache::~LLChatImageCache()
{
    if (mStatFetches > 0)
    {
        logStats("shutdown");
    }
}

// static
bool LLChatImageCache::checkUrl(const std::string& url, std::string& host, std::string& reason)
{
    host.clear();
    if (url.size() > 2048)
    {
        reason = "link too long";
        return false;
    }
    std::string head = url.substr(0, 8);
    LLStringUtil::toLower(head);
    size_t scheme_len = 0;
    if (starts_with(head, "http://"))
    {
        scheme_len = 7;
    }
    else if (starts_with(head, "https://"))
    {
        scheme_len = 8;
    }
    else
    {
        reason = "not a web link";
        return false;
    }
    for (char c : url)
    {
        const unsigned char uc = (unsigned char)c;
        if (uc <= 0x20 || uc == 0x7f || c == '\\')
        {
            reason = "invalid link";
            return false;
        }
    }
    const size_t auth_end = url.find_first_of("/?#", scheme_len);
    const std::string authority = url.substr(scheme_len, (auth_end == std::string::npos) ? std::string::npos : auth_end - scheme_len);
    if (authority.empty())
    {
        reason = "invalid link";
        return false;
    }
    if (authority.find('@') != std::string::npos)
    {
        reason = "link carries a login";
        return false;
    }
    if (authority[0] == '[')
    {
        // IPv6 literal. Our HTTP stack resolves IPv4 only, and an image host on a
        // raw IPv6 address is not worth the parsing risk.
        reason = "IP address link";
        return false;
    }
    std::string h = authority;
    const size_t colon = authority.find(':');
    if (colon != std::string::npos)
    {
        h = authority.substr(0, colon);
        const std::string port = authority.substr(colon + 1);
        if (port.empty() || port.size() > 5 ||
            !std::all_of(port.begin(), port.end(), [](char c) { return c >= '0' && c <= '9'; }) ||
            std::stoi(port) < 1 || std::stoi(port) > 65535)
        {
            reason = "invalid link";
            return false;
        }
    }
    LLStringUtil::toLower(h);
    while (!h.empty() && h.back() == '.')
    {
        h.pop_back();
    }
    if (h.empty() || h.size() > 253)
    {
        reason = "invalid link";
        return false;
    }
    for (char c : h)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
        if (!ok)
        {
            reason = "invalid link";
            return false;
        }
    }
    if (h == "localhost" || ends_with(h, ".localhost") || ends_with(h, ".local") ||
        ends_with(h, ".internal") || ends_with(h, ".lan") || ends_with(h, ".home.arpa") ||
        h.find('.') == std::string::npos)
    {
        // Single-label names resolve through the local search domain (intranet).
        reason = "local address";
        return false;
    }
    if (is_numeric_host(h))
    {
        U32 addr = 0;
        if (!parse_dotted_quad(h, addr))
        {
            reason = "IP address link";
            return false;
        }
        if (LLCore::isBlockedIPv4(addr))
        {
            reason = "private address";
            return false;
        }
    }
    host = h;
    return true;
}

// static
bool LLChatImageCache::looksLikeImage(const std::string& url, const std::string& host)
{
    const size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos)
    {
        return false;
    }
    const size_t auth_end = url.find_first_of("/?#", scheme_end + 3);
    std::string path = (auth_end == std::string::npos) ? std::string() : url.substr(auth_end);
    path = path.substr(0, path.find_first_of("?#"));
    LLStringUtil::toLower(path);

    if (ends_with(path, ".png") || ends_with(path, ".jpg") || ends_with(path, ".jpeg") || ends_with(path, ".gif") ||
        ends_with(path, ".webp"))
    {
        return true;
    }
    // Image hosts whose direct links may lack an extension; the Content-Type and
    // magic bytes decide after the fetch.
    if (host == "i.imgur.com" || host == "i.gyazo.com")
    {
        return path.size() > 1;
    }
    if (host == "cdn.discordapp.com" || host == "media.discordapp.net")
    {
        return starts_with(path, "/attachments/");
    }
    return false;
}

// static
bool LLChatImageCache::isTrustedHost(const std::string& host)
{
    static LLCachedControl<std::string> trusted_hosts(gSavedSettings, "ChatImageTrustedHosts", "");
    const std::string list = trusted_hosts();
    size_t start = 0;
    while (start <= list.size())
    {
        size_t comma = list.find(',', start);
        if (comma == std::string::npos)
        {
            comma = list.size();
        }
        std::string entry = list.substr(start, comma - start);
        LLStringUtil::trim(entry);
        LLStringUtil::toLower(entry);
        if (starts_with(entry, "*."))
        {
            entry = entry.substr(2);
        }
        else if (starts_with(entry, "."))
        {
            entry = entry.substr(1);
        }
        if (!entry.empty() && (host == entry || ends_with(host, "." + entry)))
        {
            return true;
        }
        start = comma + 1;
    }
    return false;
}

LLChatImageCache::EState LLChatImageCache::getState(const std::string& url, std::string* reason) const
{
    auto it = mEntries.find(url);
    if (it == mEntries.end())
    {
        return STATE_NONE;
    }
    if (reason)
    {
        *reason = it->second.reason;
    }
    return it->second.state;
}

LLViewerTexture* LLChatImageCache::getThumbnail(const std::string& url, S32& out_w, S32& out_h,
                                                const LLChatImageAnim** anim)
{
    if (anim)
    {
        *anim = nullptr;
    }
    auto it = mEntries.find(url);
    if (it == mEntries.end() || it->second.state != STATE_READY || it->second.thumb.isNull())
    {
        return nullptr;
    }
    it->second.last_used = nextTick();
    out_w = it->second.orig_w;
    out_h = it->second.orig_h;
    if (anim && it->second.anim.isAnimated())
    {
        *anim = &it->second.anim;
    }
    return it->second.thumb.get();
}

bool LLChatImageCache::request(const std::string& url)
{
    auto existing = mEntries.find(url);
    if (existing != mEntries.end() && existing->second.state != STATE_NONE)
    {
        existing->second.last_used = nextTick();
        return true;   // queued, loading, ready or failed already
    }
    if ((S32)mQueue.size() >= MAX_QUEUED)
    {
        // Not sticky: the widget keeps offering "Click to load".
        if (existing != mEntries.end())
        {
            mEntries.erase(existing);
        }
        LL_INFOS(LOG_TAG) << "Image preview queue full (" << MAX_QUEUED << "), not loading" << LL_ENDL;
        return false;
    }
    Entry& entry = mEntries[url];
    entry.last_used = nextTick();
    entry.state = STATE_QUEUED;
    entry.reason.clear();
    mQueue.push_back(Job{ url, 0 });
    enforceEntryCap();
    notify(url);
    pump();
    return true;
}

void LLChatImageCache::clearFailure(const std::string& url)
{
    auto it = mEntries.find(url);
    if (it != mEntries.end() && it->second.state == STATE_FAILED)
    {
        mEntries.erase(it);
        notify(url);
    }
}

U32 LLChatImageCache::requestFull(const std::string& url, const full_callback_t& cb)
{
    const U32 ticket = mNextTicket++;
    if (mNextTicket == 0)
    {
        mNextTicket = 1;
    }
    mFullCallbacks[ticket] = cb;
    mQueue.push_front(Job{ url, ticket });   // user is waiting on it
    pump();
    return ticket;
}

void LLChatImageCache::cancelFull(U32 ticket)
{
    mFullCallbacks.erase(ticket);
}

void LLChatImageCache::addFloaterBytes(S64 delta)
{
    mFloaterBytes = llmax((S64)0, mFloaterBytes + delta);
}

void LLChatImageCache::pump()
{
    while (mInFlight < MAX_IN_FLIGHT && !mQueue.empty())
    {
        Job job = mQueue.front();
        mQueue.pop_front();
        if (job.ticket == 0)
        {
            auto it = mEntries.find(job.url);
            if (it == mEntries.end() || it->second.state != STATE_QUEUED)
            {
                continue;   // dropped while queued
            }
            setState(job.url, STATE_FETCHING);
        }
        else if (mFullCallbacks.find(job.ticket) == mFullCallbacks.end())
        {
            continue;       // floater closed before the fetch started
        }
        startJob(job);
    }
}

void LLChatImageCache::startJob(const Job& job)
{
    ++mInFlight;
    ++mStatFetches;
    LLCoros::instance().launch("LLChatImageCache::fetchCoro",
                               [job]() { LLChatImageCache::fetchCoro(job); });
}

// static
void LLChatImageCache::fetchCoro(Job job)
{
    FetchOutcome fetched = fetchBytes(job.url);
    if (LLApp::isExiting() || !instanceExists())
    {
        return;
    }
    LLChatImageCache& self = instance();
    if (!fetched.ok)
    {
        self.onFetchFailed(job, fetched.reason);
        return;
    }
    if (job.ticket == 0)
    {
        self.setState(job.url, STATE_DECODING);
    }

    DecodeParams params;
    params.box_w = params.box_h = params.pot_max = FULL_MAX_SIDE;
    params.anim_box_w = params.anim_box_h = FULL_ANIM_MAX_SIDE;
    params.anim_max_bytes = FULL_ANIM_MAX_BYTES;
    if (job.ticket == 0)
    {
        static LLCachedControl<S32> max_height(gSavedSettings, "ChatImagePreviewMaxHeight", 160);
        const S32 height = llclamp((S32)max_height(), 64, 400);
        params.box_h = 2 * height;   // 2x for hi-DPI
        params.box_w = THUMB_MAX_WIDTH;
        params.pot_max = 1024;
        // Animated frames at 1x (a typical animation then fits the budget), all of
        // them together at most half the budget.
        params.anim_box_h = height;
        params.anim_box_w = THUMB_MAX_WIDTH / 2;
        params.anim_max_bytes = llmin(THUMB_ANIM_MAX_BYTES, self.budgetBytes() / 2);
    }

    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!main_queue || !general_queue)
    {
        self.onFetchFailed(job, "busy");
        return;
    }
    const std::shared_ptr<std::vector<U8> > bytes = fetched.bytes;
    const std::string mime = fetched.mime;
    const bool posted = main_queue->postTo(
        general_queue,
        // Worker: never let an exception escape (an empty exception_ptr rethrown in
        // the main loop crashed before, see LLViewerFetchedTexture::applyVKDemand).
        [bytes, mime, params]() -> DecodeOutcome
        {
            try
            {
                return decode(bytes, mime, params);
            }
            catch (const std::exception& e)
            {
                LL_WARNS_ONCE(LOG_TAG) << "decode threw: " << e.what() << LL_ENDL;
            }
            catch (...)
            {
                LL_WARNS_ONCE(LOG_TAG) << "decode threw a non-std exception" << LL_ENDL;
            }
            DecodeOutcome failed;
            failed.reason = "could not decode";
            return failed;
        },
        [job, fetched](DecodeOutcome out)   // main thread
        {
            if (LLApp::isExiting() || !instanceExists())
            {
                return;
            }
            instance().onDecoded(job, out, fetched);
        });
    if (!posted)
    {
        self.onFetchFailed(job, "busy");
    }
}

// static
LLChatImageCache::FetchOutcome LLChatImageCache::fetchBytes(const std::string& url)
{
    FetchOutcome out;

    LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter(
        new LLCoreHttpUtil::HttpCoroutineAdapter("ChatImageFetch", policy));
    LLCore::HttpRequest::ptr_t request(new LLCore::HttpRequest);
    LLCore::HttpOptions::ptr_t options(new LLCore::HttpOptions);
    LLCore::HttpHeaders::ptr_t headers(new LLCore::HttpHeaders);

    options->setWantHeaders(true);
    options->setFollowRedirects(false);       // every hop is re-checked below
    options->setRetries(0);
    options->setUseRetryAfter(false);
    options->setTimeout(TIMEOUT_SECS);
    options->setTransferTimeout(TIMEOUT_SECS);
    options->setMaxBodyBytes(MAX_BODY_BYTES);
    options->setBlockPrivateAddresses(true);

    headers->append(HTTP_OUT_HEADER_ACCEPT, "image/png,image/jpeg;q=0.9,image/webp;q=0.9,image/gif;q=0.8,*/*;q=0.1");
    headers->append(HTTP_OUT_HEADER_USER_AGENT, user_agent());
    headers->append(HTTP_OUT_HEADER_COOKIE, "");   // no cookies (as mimeDiscoveryCoro)

    std::string current = url;
    for (S32 hop = 0; hop <= MAX_REDIRECTS; ++hop)
    {
        std::string host, reason;
        if (!checkUrl(current, host, reason))
        {
            out.reason = (hop == 0) ? reason : "redirected to a blocked address";
            return out;
        }

        LLSD result = adapter->getRawAndSuspend(request, current, options, headers);
        if (LLApp::isExiting())
        {
            out.reason = "cancelled";
            return out;
        }

        const LLSD& http_results = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http_results);
        const LLSD& reply_headers = http_results[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_HEADERS];

        if (status)
        {
            const std::string mime = normalize_mime(reply_headers.has(HTTP_IN_HEADER_CONTENT_TYPE)
                                                    ? reply_headers[HTTP_IN_HEADER_CONTENT_TYPE].asString()
                                                    : std::string());
            const std::string canonical = canonical_mime(mime);
            if (canonical.empty())
            {
                out.reason = starts_with(mime, "image/") ? "unsupported format" : "not an image";
                return out;
            }
            const LLSD& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW];
            if (!raw.isBinary() || raw.asBinary().empty())
            {
                out.reason = "empty reply";
                return out;
            }
            if (raw.asBinary().size() > MAX_BODY_BYTES)
            {
                out.reason = "too large (over 10 MB)";
                return out;
            }
            out.bytes = std::make_shared<std::vector<U8> >(raw.asBinary());
            out.mime = canonical;
            out.ok = true;
            return out;
        }

        if (status.isHttpStatus())
        {
            const S32 code = (S32)status.getType();
            if (code == 301 || code == 302 || code == 303 || code == 307 || code == 308)
            {
                const std::string location = reply_headers.has(HTTP_IN_HEADER_LOCATION)
                                             ? reply_headers[HTTP_IN_HEADER_LOCATION].asString()
                                             : std::string();
                const std::string next = location.empty() ? std::string() : resolve_location(current, location);
                if (next.empty())
                {
                    out.reason = "bad redirect";
                    return out;
                }
                current = next;
                continue;
            }
            out.reason = llformat("HTTP error %d", code);
            return out;
        }

        if (status.getType() == LLCore::HttpStatus::EXT_CURL_EASY)
        {
            switch (status.getStatus())
            {
            case 63: out.reason = "too large (over 10 MB)"; break;     // CURLE_FILESIZE_EXCEEDED
            case 28: out.reason = "timed out"; break;                  // CURLE_OPERATION_TIMEDOUT
            case 6:  out.reason = "site not found"; break;             // CURLE_COULDNT_RESOLVE_HOST
            case 7:  out.reason = "could not connect"; break;          // CURLE_COULDNT_CONNECT (or private address refused)
            case 35: case 51: case 58: case 60:
                     out.reason = "secure connection failed"; break;
            default: out.reason = "network error"; break;
            }
            return out;
        }
        out.reason = "network error";
        return out;
    }
    out.reason = "too many redirects";
    return out;
}

// static (worker thread)
LLChatImageCache::DecodeOutcome LLChatImageCache::decode(const std::shared_ptr<std::vector<U8> >& bytes,
                                                         const std::string& mime,
                                                         const DecodeParams& params)
{
    const S32 box_w = params.box_w, box_h = params.box_h, pot_max = params.pot_max;
    DecodeOutcome out;
    if (!bytes || bytes->empty())
    {
        out.reason = "empty reply";
        return out;
    }
    const std::vector<U8>& data = *bytes;

    // Magic bytes must agree with the Content-Type.
    const EMagic magic = sniff(data);
    if (magic == MAGIC_GIF || magic == MAGIC_WEBP)
    {
        const bool gif = (magic == MAGIC_GIF);
        if (mime != (gif ? "image/gif" : "image/webp"))
        {
            out.reason = "not a valid image";
            return out;
        }
        // Header size check (GIF logical screen / WebP canvas), then Wuffs or
        // libwebp; frames come out fitted and power-of-two stretched like the PNG
        // path below. Both decoders check the size again themselves.
        U32 hdr_w = 0, hdr_h = 0;
        bool webp_animated = false;
        const bool hdr_ok = gif ? LLChatImageGif::headerDims(data.data(), data.size(), hdr_w, hdr_h)
                                : LLChatImageWebP::headerDims(data.data(), data.size(), hdr_w, hdr_h, webp_animated);
        if (!hdr_ok || hdr_w == 0 || hdr_h == 0)
        {
            out.reason = "not a valid image";
            return out;
        }
        if (hdr_w > (U32)MAX_DIMENSION || hdr_h > (U32)MAX_DIMENSION)
        {
            out.reason = llformat("too large (%u x %u)", hdr_w, hdr_h);
            return out;
        }
        LLChatImageGif::Params gp;
        gp.still_box_w = box_w;
        gp.still_box_h = box_h;
        gp.anim_box_w = params.anim_box_w;
        gp.anim_box_h = params.anim_box_h;
        gp.pot_max = pot_max;
        gp.max_dim = MAX_DIMENSION;
        gp.max_frame_bytes = llmax((U64)1, params.anim_max_bytes);
        LLChatImageGif::Result anim;
        if (gif)
        {
            LLChatImageGif::decode(data.data(), data.size(), gp, anim);
        }
        else
        {
            LLChatImageWebP::decode(data.data(), data.size(), gp, anim);
        }
        if (!anim.ok || anim.frames.empty())
        {
            out.reason = anim.reason.empty() ? std::string("could not decode") : anim.reason;
            return out;
        }
        out.raw = anim.frames[0];
        out.frames = std::move(anim.frames);
        out.delays = std::move(anim.delays);
        out.loops = anim.loops;
        out.truncated = anim.truncated;
        out.orig_w = anim.orig_w;
        out.orig_h = anim.orig_h;
        out.ok = true;
        return out;
    }
    const bool png = (magic == MAGIC_PNG);
    if ((png && mime != "image/png") || (magic == MAGIC_JPEG && mime != "image/jpeg") || magic == MAGIC_UNKNOWN)
    {
        out.reason = "not a valid image";
        return out;
    }

    // Size limit from the header, before the decoder allocates anything.
    U32 hdr_w = 0, hdr_h = 0;
    if (!(png ? png_dims(data, hdr_w, hdr_h) : jpeg_dims(data, hdr_w, hdr_h)))
    {
        out.reason = "not a valid image";
        return out;
    }
    if (hdr_w == 0 || hdr_h == 0)
    {
        out.reason = "not a valid image";
        return out;
    }
    if (hdr_w > (U32)MAX_DIMENSION || hdr_h > (U32)MAX_DIMENSION)
    {
        out.reason = llformat("too large (%u x %u)", hdr_w, hdr_h);
        return out;
    }

    LLPointer<LLImageFormatted> formatted;
    if (png)
    {
        formatted = new LLImagePNG();
    }
    else
    {
        formatted = new LLImageJPEG();
    }
    // Copy the bytes into the image (the source buffer is not retained), then parse the header.
    U8* dst = formatted->allocateData((S32)data.size());
    if (!dst)
    {
        out.reason = "out of memory";
        return out;
    }
    memcpy(dst, data.data(), data.size());
    if (!formatted->updateData())
    {
        out.reason = "not a valid image";
        return out;
    }
    if (formatted->getWidth() != (S32)hdr_w || formatted->getHeight() != (S32)hdr_h)
    {
        out.reason = "not a valid image";
        return out;
    }

    LLPointer<LLImageRaw> raw = new LLImageRaw();
    if (!formatted->decode(raw, 0.f) || raw->isBufferInvalid() || raw->getWidth() <= 0 || raw->getHeight() <= 0)
    {
        out.reason = "could not decode";
        return out;
    }
    formatted = nullptr;   // free the compressed copy before scaling

    // Grey + alpha -> RGBA (LLImageRaw::scale handles 1, 3 and 4 components).
    S32 comps = raw->getComponents();
    if (comps == 2)
    {
        const S32 w = raw->getWidth(), h = raw->getHeight();
        LLPointer<LLImageRaw> rgba = new LLImageRaw((U16)w, (U16)h, 4);
        if (rgba->isBufferInvalid())
        {
            out.reason = "out of memory";
            return out;
        }
        const U8* src = raw->getData();
        U8* dst = rgba->getData();
        const size_t n = (size_t)w * (size_t)h;
        for (size_t i = 0; i < n; ++i)
        {
            dst[i * 4 + 0] = dst[i * 4 + 1] = dst[i * 4 + 2] = src[i * 2 + 0];
            dst[i * 4 + 3] = src[i * 2 + 1];
        }
        raw = rgba;
        comps = 4;
    }
    if (comps != 1 && comps != 3 && comps != 4)
    {
        out.reason = "unsupported format";
        return out;
    }

    out.orig_w = raw->getWidth();
    out.orig_h = raw->getHeight();

    // Fit into the box (never upscale), halving first so the final bilinear pass
    // does not alias, then stretch to power-of-two dims (LLTextureData requires them;
    // the widget draws into an aspect-correct rect).
    const F32 fit = llmin(1.f, llmin((F32)box_w / (F32)out.orig_w, (F32)box_h / (F32)out.orig_h));
    const S32 target_w = llmax(1, ll_round((F32)out.orig_w * fit));
    const S32 target_h = llmax(1, ll_round((F32)out.orig_h * fit));
    S32 w = out.orig_w, h = out.orig_h;
    while (w / 2 >= target_w && h / 2 >= target_h && w >= 2 && h >= 2)
    {
        w /= 2;
        h /= 2;
        if (!raw->scale(w, h))
        {
            out.reason = "could not decode";
            return out;
        }
    }
    const S32 pot_w = LLImageRaw::biasedDimToPowerOfTwo(target_w, pot_max);
    const S32 pot_h = LLImageRaw::biasedDimToPowerOfTwo(target_h, pot_max);
    if (!raw->scale(pot_w, pot_h))
    {
        out.reason = "could not decode";
        return out;
    }
    out.raw = raw;
    out.ok = true;
    return out;
}

void LLChatImageCache::onFetchFailed(const Job& job, const std::string& reason)
{
    ++mStatFailures;
    LL_INFOS(LOG_TAG) << "Image preview unavailable (" << reason << ")" << LL_ENDL;
    if (job.ticket != 0)
    {
        auto it = mFullCallbacks.find(job.ticket);
        if (it != mFullCallbacks.end())
        {
            full_callback_t cb = it->second;
            mFullCallbacks.erase(it);
            FullResult result;
            result.reason = reason;
            cb(result);
        }
    }
    else
    {
        setState(job.url, STATE_FAILED, reason);
    }
    finishJob();
}

void LLChatImageCache::onDecoded(const Job& job, const DecodeOutcome& out, const FetchOutcome& fetched)
{
    if (!out.ok)
    {
        onFetchFailed(job, out.reason);
        return;
    }
    if (job.ticket != 0)
    {
        auto it = mFullCallbacks.find(job.ticket);
        if (it != mFullCallbacks.end())
        {
            full_callback_t cb = it->second;
            mFullCallbacks.erase(it);
            FullResult result;
            result.ok = true;
            result.raw = out.raw;
            if (out.frames.size() > 1)
            {
                result.frames = out.frames;
                result.delays = out.delays;
                result.loops = out.loops;
                result.truncated = out.truncated;
            }
            result.orig_w = out.orig_w;
            result.orig_h = out.orig_h;
            result.mime = fetched.mime;
            result.bytes = fetched.bytes;
            cb(result);
        }
        finishJob();
        return;
    }

    auto it = mEntries.find(job.url);
    if (it == mEntries.end())
    {
        finishJob();   // dropped meanwhile
        return;
    }
    LLPointer<LLViewerTexture> tex = LLViewerTextureManager::getLocalTexture(out.raw.get(), false);
    if (tex.isNull())
    {
        onFetchFailed(job, "could not decode");
        return;
    }
    tex->setBoostLevel(LLGLTexture::BOOST_THUMBNAIL);   // VKMem "thumb" bucket
    U64 bytes = (U64)out.raw->getWidth() * (U64)out.raw->getHeight() * (U64)out.raw->getComponents();

    // Animated GIF / WebP: one texture per frame (same creation path), all counted.
    LLChatImageAnim anim;
    if (out.frames.size() > 1 && out.delays.size() == out.frames.size())
    {
        anim.frames.reserve(out.frames.size());
        anim.frames.push_back(tex);
        for (size_t i = 1; i < out.frames.size(); ++i)
        {
            const LLImageRaw* frame = out.frames[i].get();
            LLPointer<LLViewerTexture> frame_tex = frame ? LLViewerTextureManager::getLocalTexture(frame, false)
                                                         : LLPointer<LLViewerTexture>();
            if (frame_tex.isNull())
            {
                break;   // keep the frames made so far
            }
            frame_tex->setBoostLevel(LLGLTexture::BOOST_THUMBNAIL);
            anim.frames.push_back(frame_tex);
            bytes += (U64)frame->getWidth() * (U64)frame->getHeight() * (U64)frame->getComponents();
        }
        anim.delays.assign(out.delays.begin(), out.delays.begin() + anim.frames.size());
        anim.loops = out.loops;
        anim.updateDuration();
        if (!anim.isAnimated())
        {
            anim.clear();
        }
        LL_DEBUGS(LOG_TAG) << "Animated preview: " << anim.frames.size() << " frames, "
                           << anim.duration << " s per loop, loops=" << anim.loops
                           << (out.truncated ? " (cut short by the limits)" : "") << ", "
                           << (bytes / 1024) << " KB" << LL_ENDL;
    }

    Entry& entry = it->second;
    entry.thumb = tex;
    entry.anim = std::move(anim);
    entry.orig_w = out.orig_w;
    entry.orig_h = out.orig_h;
    entry.bytes = bytes;
    entry.last_used = nextTick();
    entry.state = STATE_READY;
    entry.reason.clear();
    mThumbBytes += entry.bytes;
    mPeakThumbBytes = llmax(mPeakThumbBytes, mThumbBytes);
    notify(job.url);
    enforceBudget(job.url);
    finishJob();
}

void LLChatImageCache::finishJob()
{
    if (mInFlight > 0)
    {
        --mInFlight;
    }
    pump();
}

void LLChatImageCache::setState(const std::string& url, EState state, const std::string& reason)
{
    Entry& entry = mEntries[url];
    if (entry.state == state && entry.reason == reason)
    {
        return;
    }
    if (entry.state == STATE_READY && state != STATE_READY)
    {
        mThumbBytes -= llmin(mThumbBytes, entry.bytes);
        entry.bytes = 0;
    }
    LLPointer<LLViewerTexture> keep_alive = entry.thumb;   // released after the notify
    LLChatImageAnim keep_anim;
    if (state != STATE_READY)
    {
        entry.thumb = nullptr;
        keep_anim = std::move(entry.anim);
        entry.anim.clear();
    }
    entry.state = state;
    entry.reason = reason;
    notify(url);
}

void LLChatImageCache::notify(const std::string& url)
{
    mUpdateSignal(url);
}

U64 LLChatImageCache::budgetBytes() const
{
    static LLCachedControl<S32> budget_mb(gSavedSettings, "ChatImageCacheMB", 64);
    return (U64)llclamp((S32)budget_mb(), 1, 1024) * 1024 * 1024;
}

void LLChatImageCache::enforceBudget(const std::string& keep_url)
{
    const U64 budget = budgetBytes();
    U32 evicted = 0;
    while (mThumbBytes > budget)
    {
        auto victim = mEntries.end();
        for (auto it = mEntries.begin(); it != mEntries.end(); ++it)
        {
            if (it->second.state == STATE_READY && it->first != keep_url &&
                (victim == mEntries.end() || it->second.last_used < victim->second.last_used))
            {
                victim = it;
            }
        }
        if (victim == mEntries.end())
        {
            break;   // only the newest is left; a single thumbnail may exceed a tiny budget
        }
        // Drop the entry first, notify (widgets switch to "Click to reload" and dirty
        // their retained floater), then release the texture.
        const std::string url = victim->first;
        LLPointer<LLViewerTexture> keep_alive = victim->second.thumb;
        LLChatImageAnim keep_anim = std::move(victim->second.anim);
        mThumbBytes -= llmin(mThumbBytes, victim->second.bytes);
        mEntries.erase(victim);
        ++mStatEvictions;
        ++evicted;
        notify(url);
    }
    if (evicted > 0)
    {
        logStats(llformat("evicted %u", evicted));
    }
}

void LLChatImageCache::enforceEntryCap()
{
    if ((S32)mEntries.size() <= MAX_ENTRIES)
    {
        return;
    }
    // Drop the least recently used settled entries (never queued / in flight) down
    // to 90% of the cap.
    std::vector<std::pair<U64, std::string> > candidates;
    candidates.reserve(mEntries.size());
    for (const auto& kv : mEntries)
    {
        const EState s = kv.second.state;
        if (s == STATE_NONE || s == STATE_FAILED || s == STATE_READY)
        {
            candidates.emplace_back(kv.second.last_used, kv.first);
        }
    }
    std::sort(candidates.begin(), candidates.end());
    const size_t target = (size_t)MAX_ENTRIES * 9 / 10;
    for (const auto& c : candidates)
    {
        if (mEntries.size() <= target)
        {
            break;
        }
        auto it = mEntries.find(c.second);
        if (it == mEntries.end())
        {
            continue;
        }
        LLPointer<LLViewerTexture> keep_alive = it->second.thumb;
        LLChatImageAnim keep_anim = std::move(it->second.anim);
        if (it->second.state == STATE_READY)
        {
            mThumbBytes -= llmin(mThumbBytes, it->second.bytes);
        }
        mEntries.erase(it);
        notify(c.second);
    }
}

void LLChatImageCache::logStats(const std::string& why) const
{
    LL_INFOS(LOG_TAG) << "Chat image previews (" << why << "): thumbnails live="
                      << (mThumbBytes / 1024) << " KB cap=" << (budgetBytes() / 1024)
                      << " KB peak=" << (mPeakThumbBytes / 1024) << " KB"
                      << ", entries=" << mEntries.size() << "/" << MAX_ENTRIES
                      << ", in flight=" << mInFlight << ", queued=" << mQueue.size()
                      << ", preview floaters=" << (mFloaterBytes / 1024) << " KB"
                      << ", fetches=" << mStatFetches << " failures=" << mStatFailures
                      << " evictions=" << mStatEvictions << LL_ENDL;
}
