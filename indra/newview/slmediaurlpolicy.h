/**
 * @file slmediaurlpolicy.h
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

#ifndef SL_MEDIAURLPOLICY_H
#define SL_MEDIAURLPOLICY_H

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// Parcel music, parcel media and media on prims / HUDs carry urls chosen by land and
// object owners. Before such a url goes to a media plugin (or to the MIME type probe),
// its host must be a public address: not loopback, a private / link-local / CGNAT
// network, unspecified, multicast or broadcast, nor a name like "localhost" or
// "*.local". Host names are resolved off the main thread and EVERY resolved address
// must be public. MediaAllowPrivateAddresses (default off) lifts the restriction for
// people who stream from their own network.
//
// Limits (the plugin opens its own connections afterwards): redirects the plugin
// follows, a host whose DNS answer changes between this check and the plugin's own
// lookup, and pages / subresources a web page loads in the browser plugin are not
// covered here.
namespace SLMediaURLPolicy
{
    enum class Verdict
    {
        ALLOWED,
        BLOCKED,
        PENDING     // a lookup is running: on_done gets the answer later, on the main thread
    };

    using callback_t = std::function<void(bool allowed)>;

    // Main thread only. ALLOWED / BLOCKED when the answer is known now (setting on, no
    // host, IP literal, special name, cached lookup); on_done is not called then.
    // PENDING: on_done(allowed) runs later on the main thread, never from inside check().
    // "what" names the media for the log and the one-time notification ("Parcel music").
    // A refusal is logged; the first refusal of a host per session also notifies.
    Verdict check(const std::string& url, const std::string& what, callback_t on_done);

    // ---- Pure helpers (no viewer state; exposed for tests) ----

    // Every host a URL parser could take from url, lower-cased in no particular order;
    // parsers disagree on '\' and repeated '@', so each reading is listed. Empty for urls
    // with no host (data:, about:). False: no usable host could be found (malformed).
    bool extractHosts(const std::string& url, std::vector<std::string>& hosts);

    enum class HostClass
    {
        PUBLIC,     // an IP literal in a public range
        BLOCKED,    // non-public literal, local name, or a host form we will not interpret
        RESOLVE     // a host name: decided by what it resolves to
    };
    // host as extractHosts() returns it (an IPv6 literal keeps its brackets).
    HostClass classifyHost(const std::string& host, std::string& reason);

    // IPv4 literal in any form a URL parser or inet_aton() accepts: 1-4 parts, each
    // decimal, octal (leading 0) or hex (0x), the last one filling the remaining bytes
    // ("2130706433", "0x7f.1", "0177.0.0.1"). Result in host byte order.
    bool parseIPv4Literal(const std::string& host, uint32_t& address);
}

#endif // SL_MEDIAURLPOLICY_H
