/**
 * @file httpaddresspolicy.h
 * @brief Which IP addresses an untrusted URL may reach.
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

#ifndef _LLCORE_HTTP_ADDRESS_POLICY_H_
#define _LLCORE_HTTP_ADDRESS_POLICY_H_

#include <cstdint>

namespace LLCore
{

// True for IPv4 addresses (host byte order) that a fetch of an untrusted URL must not
// reach: "this" network 0/8, loopback 127/8, RFC 1918 private (10/8, 172.16/12,
// 192.168/16), link-local 169.254/16, CGNAT 100.64/10, 192.0.0/24, benchmarking
// 198.18/15, multicast, reserved and broadcast (224/3).
bool isBlockedIPv4(uint32_t address);

// Same for IPv6 (16 bytes, network order): unspecified, loopback, unique-local
// fc00::/7, link-local fe80::/10, old site-local fec0::/10, multicast ff00::/8,
// local-use NAT64 64:ff9b:1::/48, and IPv4-mapped (::ffff:0:0/96), IPv4-compatible
// (::/96), NAT64 (64:ff9b::/96), 6to4 (2002::/16) and Teredo (2001::/32, client
// address) embeddings of a blocked IPv4 address.
bool isBlockedIPv6(const unsigned char* address);

} // end namespace LLCore

#endif // _LLCORE_HTTP_ADDRESS_POLICY_H_
