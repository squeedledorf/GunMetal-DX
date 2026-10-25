/**
 * @file slmediaurlpolicy.cpp
 * @brief Media urls from the world may only reach public network addresses.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#include "slmediaurlpolicy.h"

#include "httpaddresspolicy.h"
#include "llapp.h"
#include "llframetimer.h"
#include "llnotificationsutil.h"
#include "llviewercontrol.h"
#include "workqueue.h"

#if LL_WINDOWS
#include <llwin32headers.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif

#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <set>

namespace
{

const char* const LOG_TAG = "Media";

bool ends_with(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

bool is_hex(char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

int hex_value(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return c - 'A' + 10;
}

// One IPv4 part: decimal, octal (leading 0) or hex (0x, possibly with no digits = 0).
bool parse_ipv4_part(const std::string& part, uint64_t& out)
{
    if (part.empty())
    {
        return false;
    }
    uint64_t value = 0;
    size_t i = 0;
    int base = 10;
    if (part.size() >= 2 && part[0] == '0' && (part[1] == 'x' || part[1] == 'X'))
    {
        base = 16;
        i = 2;
    }
    else if (part.size() >= 2 && part[0] == '0')
    {
        base = 8;
        i = 1;
    }
    for (; i < part.size(); ++i)
    {
        const char c = part[i];
        int digit = 0;
        if (base == 16 && is_hex(c))
        {
            digit = hex_value(c);
        }
        else if (c >= '0' && c <= '9' && (base == 10 || c <= '7'))
        {
            digit = c - '0';
        }
        else
        {
            return false;
        }
        value = value * uint64_t(base) + uint64_t(digit);
        if (value > 0xffffffffull)
        {
            return false;
        }
    }
    out = value;
    return true;
}

// WHATWG "ends in a number": the last label is all decimal digits, or 0x + hex digits.
// Such a host is an IPv4 address to a browser; anything we cannot read as one is refused.
bool ends_in_number(const std::string& host)
{
    const size_t dot = host.rfind('.');
    const std::string last = (dot == std::string::npos) ? host : host.substr(dot + 1);
    if (last.empty())
    {
        return false;
    }
    if (last.size() >= 2 && last[0] == '0' && (last[1] == 'x' || last[1] == 'X'))
    {
        return std::all_of(last.begin() + 2, last.end(), is_hex);
    }
    return std::all_of(last.begin(), last.end(), [](char c) { return c >= '0' && c <= '9'; });
}

bool percent_decode(const std::string& in, std::string& out)
{
    out.clear();
    for (size_t i = 0; i < in.size(); ++i)
    {
        if (in[i] != '%')
        {
            out += in[i];
            continue;
        }
        if (i + 2 >= in.size() || !is_hex(in[i + 1]) || !is_hex(in[i + 2]))
        {
            return false;
        }
        out += char(hex_value(in[i + 1]) * 16 + hex_value(in[i + 2]));
        i += 2;
    }
    return true;
}

// A host that is safe to show in a notification as-is.
std::string displayable_host(const std::string& host)
{
    if (host.empty() || host.size() > 100)
    {
        return "(unusual host name)";
    }
    for (char c : host)
    {
        const unsigned char uc = (unsigned char)c;
        if (uc <= 0x20 || uc >= 0x7f)
        {
            return "(unusual host name)";
        }
    }
    return host;
}

// ---- Main-thread state ----

struct CacheEntry
{
    bool allowed = false;
    F64 expires = 0.0;
};

struct Join
{
    size_t remaining = 0;
    bool blocked = false;
    SLMediaURLPolicy::callback_t on_done;
};

struct Lookup
{
    std::string what;                       // for the notification
    std::vector<std::shared_ptr<Join> > joins;
};

struct ResolveOutcome
{
    bool resolved = false;      // at least one address came back
    bool blocked = false;       // some address is not public
    std::string detail;         // the first non-public address, or the resolver error
};

constexpr size_t MAX_CACHE = 256;
constexpr F64 ALLOWED_TTL = 300.0;   // a navigate re-checks after the MIME probe: keep this well above it
constexpr F64 BLOCKED_TTL = 60.0;
constexpr F64 FAILED_TTL = 15.0;     // lookup errors: retried soon (a transient DNS failure)

std::map<std::string, CacheEntry> sCache;
std::map<std::string, Lookup> sLookups;
std::set<std::string> sNotifiedHosts;

void trim_cache(F64 now)
{
    if (sCache.size() < MAX_CACHE)
    {
        return;
    }
    for (auto it = sCache.begin(); it != sCache.end();)
    {
        it = (it->second.expires <= now) ? sCache.erase(it) : std::next(it);
    }
    if (sCache.size() >= MAX_CACHE)
    {
        sCache.clear();
    }
}

void report_blocked(const std::string& what, const std::string& host, const std::string& reason, bool notify)
{
    if (!notify)
    {
        LL_WARNS(LOG_TAG) << what << " refused: host " << host << " " << reason << LL_ENDL;
        return;
    }
    LL_WARNS(LOG_TAG) << what << " refused: host " << host << " " << reason
                      << " (MediaAllowPrivateAddresses is off)" << LL_ENDL;
    if (sNotifiedHosts.size() >= MAX_CACHE)
    {
        sNotifiedHosts.clear();
    }
    if (!sNotifiedHosts.insert(host).second)
    {
        return;   // one notification per host per session
    }
    LLSD args;
    args["WHAT"] = what;
    args["HOST"] = displayable_host(host);
    LLNotificationsUtil::add("MediaPrivateAddressBlocked", args);
}

// Worker thread: resolve host and test every address.
ResolveOutcome resolve_host(const std::string& host)
{
    ResolveOutcome out;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo* results = nullptr;
    const int err = getaddrinfo(host.c_str(), nullptr, &hints, &results);
    if (err != 0 || !results)
    {
        out.detail = "could not be resolved (error " + std::to_string(err) + ")";
        if (results)
        {
            freeaddrinfo(results);
        }
        return out;
    }
    for (const struct addrinfo* ai = results; ai; ai = ai->ai_next)
    {
        char text[INET6_ADDRSTRLEN] = { 0 };
        bool blocked = false;
        if (ai->ai_family == AF_INET && ai->ai_addrlen >= sizeof(struct sockaddr_in))
        {
            const struct sockaddr_in* sin = reinterpret_cast<const struct sockaddr_in*>(ai->ai_addr);
            out.resolved = true;
            blocked = LLCore::isBlockedIPv4(ntohl(sin->sin_addr.s_addr));
            if (blocked)
            {
                inet_ntop(AF_INET, (void*)&sin->sin_addr, text, sizeof(text));
            }
        }
        else if (ai->ai_family == AF_INET6 && ai->ai_addrlen >= sizeof(struct sockaddr_in6))
        {
            const struct sockaddr_in6* sin6 = reinterpret_cast<const struct sockaddr_in6*>(ai->ai_addr);
            out.resolved = true;
            blocked = LLCore::isBlockedIPv6(reinterpret_cast<const unsigned char*>(&sin6->sin6_addr));
            if (blocked)
            {
                inet_ntop(AF_INET6, (void*)&sin6->sin6_addr, text, sizeof(text));
            }
        }
        if (blocked && !out.blocked)
        {
            out.blocked = true;
            out.detail = std::string("resolves to the non-public address ") + text;
        }
    }
    freeaddrinfo(results);
    if (!out.resolved)
    {
        out.detail = "resolved to no IP address";
    }
    return out;
}

void finish_join(const std::shared_ptr<Join>& join, bool blocked)
{
    join->blocked = join->blocked || blocked;
    if (--join->remaining == 0 && join->on_done)
    {
        SLMediaURLPolicy::callback_t done = std::move(join->on_done);
        join->on_done = nullptr;
        done(!join->blocked);
    }
}

// Main thread: a lookup finished.
void on_resolved(const std::string& host, const ResolveOutcome& outcome)
{
    if (LLApp::isExiting())
    {
        return;
    }
    const auto found = sLookups.find(host);
    if (found == sLookups.end())
    {
        return;
    }
    Lookup lookup = std::move(found->second);
    sLookups.erase(found);

    const bool allowed = outcome.resolved && !outcome.blocked;
    const F64 now = LLFrameTimer::getTotalSeconds();
    trim_cache(now);
    CacheEntry& entry = sCache[host];
    entry.allowed = allowed;
    entry.expires = now + (allowed ? ALLOWED_TTL : (outcome.blocked ? BLOCKED_TTL : FAILED_TTL));

    if (!allowed)
    {
        // A name that does not resolve is not a private-address case: log it only.
        report_blocked(lookup.what, host, outcome.detail, outcome.blocked);
    }
    for (const std::shared_ptr<Join>& join : lookup.joins)
    {
        finish_join(join, !allowed);
    }
}

} // end anonymous namespace

namespace SLMediaURLPolicy
{

bool parseIPv4Literal(const std::string& host, uint32_t& address)
{
    std::vector<uint64_t> parts;
    size_t start = 0;
    while (true)
    {
        const size_t dot = host.find('.', start);
        const std::string part = host.substr(start, (dot == std::string::npos) ? std::string::npos : dot - start);
        uint64_t value = 0;
        if (!parse_ipv4_part(part, value) || parts.size() >= 4)
        {
            return false;
        }
        parts.push_back(value);
        if (dot == std::string::npos)
        {
            break;
        }
        start = dot + 1;
    }
    uint64_t result = 0;
    for (size_t i = 0; i + 1 < parts.size(); ++i)
    {
        if (parts[i] > 255)
        {
            return false;
        }
        result = (result << 8) | parts[i];
    }
    const size_t last_bytes = 5 - parts.size();   // 4, 3, 2 or 1
    const uint64_t last = parts.back();
    if (last >= (uint64_t(1) << (8 * last_bytes)))
    {
        return false;
    }
    result = (result << (8 * last_bytes)) | last;
    address = uint32_t(result);
    return true;
}

bool extractHosts(const std::string& raw_url, std::vector<std::string>& hosts)
{
    hosts.clear();
    size_t begin = 0;
    size_t end = raw_url.size();
    while (begin < end && (unsigned char)raw_url[begin] <= 0x20) ++begin;
    while (end > begin && (unsigned char)raw_url[end - 1] <= 0x20) --end;
    const std::string url = raw_url.substr(begin, end - begin);
    for (char c : url)
    {
        const unsigned char uc = (unsigned char)c;
        if (uc < 0x20 || uc == 0x7f)
        {
            return false;   // browsers drop these before parsing: a hidden host
        }
    }
    if (url.empty())
    {
        return true;
    }

    // Scheme: ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) before the first ':'.
    std::string rest = url;
    const size_t colon = url.find(':');
    if (colon != std::string::npos && colon > 1)
    {
        std::string scheme;
        bool is_scheme = true;
        for (size_t i = 0; i < colon && is_scheme; ++i)
        {
            char c = url[i];
            if (c >= 'A' && c <= 'Z') c = char(c - 'A' + 'a');
            const bool alpha = (c >= 'a' && c <= 'z');
            const bool other = (c >= '0' && c <= '9') || c == '+' || c == '-' || c == '.';
            is_scheme = alpha || (i > 0 && other);
            scheme += c;
        }
        if (is_scheme)
        {
            if (scheme == "data" || scheme == "about")
            {
                return true;   // in-memory content, no host
            }
            rest = url.substr(colon + 1);
        }
    }

    // Special-scheme parsing skips any run of '/' and '\' before the authority.
    size_t auth_begin = 0;
    while (auth_begin < rest.size() && (rest[auth_begin] == '/' || rest[auth_begin] == '\\')) ++auth_begin;

    std::set<std::string> found;
    for (const char* terminators : { "/?#\\", "/?#" })
    {
        const size_t auth_end = rest.find_first_of(terminators, auth_begin);
        const std::string authority = rest.substr(auth_begin, (auth_end == std::string::npos) ? std::string::npos : auth_end - auth_begin);
        const size_t first_at = authority.find('@');
        const size_t last_at = authority.rfind('@');
        std::vector<std::string> host_ports;
        host_ports.push_back((last_at == std::string::npos) ? authority : authority.substr(last_at + 1));
        if (first_at != last_at)
        {
            host_ports.push_back(authority.substr(first_at + 1));
        }
        for (const std::string& host_port : host_ports)
        {
            std::string host;
            if (!host_port.empty() && host_port[0] == '[')
            {
                const size_t close = host_port.find(']');
                if (close == std::string::npos)
                {
                    return false;
                }
                host = host_port.substr(0, close + 1);
            }
            else
            {
                host = host_port.substr(0, host_port.find(':'));
            }
            if (host.empty())
            {
                return false;
            }
            std::transform(host.begin(), host.end(), host.begin(),
                           [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; });
            found.insert(host);
        }
    }
    hosts.assign(found.begin(), found.end());
    return true;
}

HostClass classifyHost(const std::string& raw_host, std::string& reason)
{
    if (raw_host.size() > 255)
    {
        reason = "is too long";
        return HostClass::BLOCKED;
    }
    if (!raw_host.empty() && raw_host[0] == '[')
    {
        // IPv6 literal. A zone id ("%25eth0") only exists for link-local addresses.
        if (raw_host.size() < 3 || raw_host.back() != ']' || raw_host.find('%') != std::string::npos)
        {
            reason = "is not a usable IPv6 address";
            return HostClass::BLOCKED;
        }
        const std::string inner = raw_host.substr(1, raw_host.size() - 2);
        unsigned char bytes[16];
        if (inet_pton(AF_INET6, inner.c_str(), bytes) != 1)
        {
            reason = "is not a usable IPv6 address";
            return HostClass::BLOCKED;
        }
        if (LLCore::isBlockedIPv6(bytes))
        {
            reason = "is a non-public address";
            return HostClass::BLOCKED;
        }
        return HostClass::PUBLIC;
    }

    std::string host;
    if (!percent_decode(raw_host, host))   // browsers decode the host before reading it
    {
        reason = "is not a valid host name";
        return HostClass::BLOCKED;
    }
    std::transform(host.begin(), host.end(), host.begin(),
                   [](char c) { return (c >= 'A' && c <= 'Z') ? char(c - 'A' + 'a') : c; });
    while (!host.empty() && host.back() == '.')
    {
        host.pop_back();
    }
    if (host.empty())
    {
        reason = "is empty";
        return HostClass::BLOCKED;
    }
    for (char c : host)
    {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '.' || c == '_';
        if (!ok)
        {
            // Non-ASCII (IDN, full-width digits) and odd characters: browsers normalise
            // these into something else, which may well be an IP address.
            reason = "is not a plain ASCII host name";
            return HostClass::BLOCKED;
        }
    }
    if (host == "localhost" || ends_with(host, ".localhost") || ends_with(host, ".local") ||
        ends_with(host, ".localdomain") || host == "localdomain" || ends_with(host, ".internal") ||
        ends_with(host, ".lan") || ends_with(host, ".home.arpa"))
    {
        reason = "is a local network name";
        return HostClass::BLOCKED;
    }
    if (ends_in_number(host))
    {
        uint32_t address = 0;
        if (!parseIPv4Literal(host, address))
        {
            reason = "is not a usable IPv4 address";
            return HostClass::BLOCKED;
        }
        if (LLCore::isBlockedIPv4(address))
        {
            reason = "is a non-public address";
            return HostClass::BLOCKED;
        }
        return HostClass::PUBLIC;
    }
    return HostClass::RESOLVE;
}

Verdict check(const std::string& url, const std::string& what, callback_t on_done)
{
    static LLCachedControl<bool> allow_private(gSavedSettings, "MediaAllowPrivateAddresses", false);
    if (allow_private)
    {
        return Verdict::ALLOWED;
    }

    std::vector<std::string> hosts;
    if (!extractHosts(url, hosts))
    {
        LL_WARNS(LOG_TAG) << what << " refused: no usable host in the url" << LL_ENDL;
        return Verdict::BLOCKED;
    }

    const F64 now = LLFrameTimer::getTotalSeconds();
    std::vector<std::string> to_resolve;
    for (const std::string& host : hosts)
    {
        std::string reason;
        const HostClass host_class = classifyHost(host, reason);
        if (host_class == HostClass::BLOCKED)
        {
            report_blocked(what, host, reason, true);
            return Verdict::BLOCKED;
        }
        if (host_class == HostClass::PUBLIC)
        {
            continue;
        }
        const auto cached = sCache.find(host);
        if (cached != sCache.end() && cached->second.expires > now)
        {
            if (!cached->second.allowed)
            {
                LL_WARNS(LOG_TAG) << what << " refused: host " << host << " (recent lookup)" << LL_ENDL;
                return Verdict::BLOCKED;
            }
            continue;
        }
        to_resolve.push_back(host);
    }
    if (to_resolve.empty())
    {
        return Verdict::ALLOWED;
    }

    LL::WorkQueue::ptr_t main_queue = LL::WorkQueue::getInstance("mainloop");
    LL::WorkQueue::ptr_t general_queue = LL::WorkQueue::getInstance("General");
    if (!main_queue || !general_queue)
    {
        LL_WARNS(LOG_TAG) << what << " refused: host lookup unavailable" << LL_ENDL;
        return Verdict::BLOCKED;
    }

    std::shared_ptr<Join> join = std::make_shared<Join>();
    join->remaining = to_resolve.size();
    join->on_done = std::move(on_done);
    for (const std::string& host : to_resolve)
    {
        auto running = sLookups.find(host);
        if (running != sLookups.end())
        {
            running->second.joins.push_back(join);
            continue;
        }
        const bool posted = main_queue->postTo(
            general_queue,
            [host]() -> ResolveOutcome
            {
                try
                {
                    return resolve_host(host);
                }
                catch (...)
                {
                }
                ResolveOutcome failed;
                failed.detail = "could not be resolved";
                return failed;
            },
            [host](ResolveOutcome outcome)   // main thread
            {
                on_resolved(host, outcome);
            });
        if (!posted)
        {
            // Refuse the whole url. Lookups already started for its other hosts still
            // hold the join, so drop its callback: they must not answer for it.
            LL_WARNS(LOG_TAG) << what << " refused: host lookup for " << host << " could not start" << LL_ENDL;
            join->on_done = nullptr;
            join->remaining = size_t(-1) / 2;
            return Verdict::BLOCKED;
        }
        Lookup& lookup = sLookups[host];
        lookup.what = what;
        lookup.joins.push_back(join);
    }
    return Verdict::PENDING;
}

} // end namespace SLMediaURLPolicy
