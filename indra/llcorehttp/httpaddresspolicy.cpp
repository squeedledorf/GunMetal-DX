/**
 * @file httpaddresspolicy.cpp
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

#include "httpaddresspolicy.h"

namespace
{

uint32_t be32(const unsigned char* p)
{
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

bool all_zero(const unsigned char* p, int count)
{
    for (int i = 0; i < count; ++i)
    {
        if (p[i])
        {
            return false;
        }
    }
    return true;
}

} // end anonymous namespace

namespace LLCore
{

bool isBlockedIPv4(uint32_t a)
{
    const uint32_t b0 = a >> 24;
    const uint32_t b1 = (a >> 16) & 0xffu;
    if (b0 == 0 || b0 == 10 || b0 == 127) return true;            // 0/8, 10/8, 127/8
    if (b0 == 169 && b1 == 254) return true;                       // 169.254/16
    if (b0 == 172 && (b1 & 0xf0u) == 16) return true;              // 172.16/12
    if (b0 == 192 && b1 == 168) return true;                       // 192.168/16
    if (b0 == 100 && (b1 & 0xc0u) == 64) return true;              // 100.64/10
    const uint32_t b2 = (a >> 8) & 0xffu;
    if (b0 == 192 && b1 == 0 && b2 == 0) return true;              // 192.0.0/24 (IETF protocol)
    if (b0 == 198 && (b1 & 0xfeu) == 18) return true;              // 198.18/15 (benchmarking)
    if (b0 >= 224) return true;                                    // multicast, reserved, broadcast
    return false;
}

bool isBlockedIPv6(const unsigned char* b)
{
    if (all_zero(b, 10))
    {
        const bool mapped = (b[10] == 0xff && b[11] == 0xff);
        const bool compat = (b[10] == 0 && b[11] == 0);
        if (mapped || compat)
        {
            return isBlockedIPv4(be32(b + 12));   // also covers :: and ::1 (0.0.0.0 / 0.0.0.1)
        }
    }
    const bool nat64_prefix = (b[0] == 0 && b[1] == 0x64 && b[2] == 0xff && b[3] == 0x9b);
    // 64:ff9b::/96 (NAT64): the address embeds an IPv4 one.
    if (nat64_prefix && all_zero(b + 4, 8) && isBlockedIPv4(be32(b + 12))) return true;
    // 64:ff9b:1::/48: local-use NAT64 (RFC 8215), translates into the local network.
    if (nat64_prefix && b[4] == 0 && b[5] == 1) return true;
    // 2002::/16 (6to4): bytes 2-5 are an IPv4 address.
    if (b[0] == 0x20 && b[1] == 0x02 && isBlockedIPv4(be32(b + 2))) return true;
    // 2001:0000::/32 (Teredo): the client address is the last four bytes, inverted.
    if (b[0] == 0x20 && b[1] == 0x01 && b[2] == 0 && b[3] == 0 && isBlockedIPv4(~be32(b + 12))) return true;
    if (b[0] == 0xfeu && (b[1] & 0xc0u) == 0xc0u) return true;     // fec0::/10 (old site-local)
    if ((b[0] & 0xfeu) == 0xfcu) return true;                      // fc00::/7
    if (b[0] == 0xfeu && (b[1] & 0xc0u) == 0x80u) return true;     // fe80::/10
    if (b[0] == 0xffu) return true;                                // multicast
    return false;
}

} // end namespace LLCore
