/**
 * @file gmdxnightskygrid.h
 * @brief GunMetal-DX night sky: the star catalogue file and the star lookup grid (no viewer
 *        dependencies; scripts/nightsky/gmdx_star_grid_test.cpp checks it).
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

// <GMDX> The sky shader finds the stars near a view direction through a grid on the faces of a
// cube around the celestial sphere (J2000 equatorial frame: x toward the March equinox, z toward
// the north celestial pole). Each of the 6 x N x N cells lists up to K stars; a star is listed in
// every cell that comes within SPLAT of it, so a pixel only reads its own cell and a star's glow
// never stops at a cell edge. Brightest stars first: a crowded cell drops its faintest.
//
// Texture (RGBA8, W x H, read with texelFetch in vkclean/shaders/gmdx_night_sky.glsl, which
// must agree with everything here): face f, cell (cx, cy), slot k ->
//   texel ((cx * K + k) * 2,     f * N + cy) = the star's direction, octahedral, 16 bits a
//                                              component: (u >> 8, u & 255, v >> 8, v & 255)
//   texel ((cx * K + k) * 2 + 1, f * N + cy) = r, g, b (its colour, brightest channel 255),
//                                              V magnitude code (0 = empty slot, the list's end)
// Rows 6N..H-1 are padding (power-of-two size).
//
// Catalogue file (scripts/nightsky/gmdx_night_sky_assets.py writes it from the Yale Bright Star
// Catalogue): "GMDXBSC1", uint32 count, then per star uint16 RA, int16 Dec, uint8 magnitude
// code, uint8 r, g, b (little-endian).

#ifndef GMDX_NIGHTSKYGRID_H
#define GMDX_NIGHTSKYGRID_H

#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace GMDXStarGrid
{
constexpr int N = 64;                       // cells per cube-face edge (1.8 deg at a face centre, 0.8 at a corner)
constexpr int K = 8;                        // stars per cell
constexpr int W = N * K * 2;                // 1024
constexpr int H = 512;                      // 6 * N = 384 rows, padded
constexpr float SPLAT = 0.5f * 3.14159265f / 180.f;   // the widest star glow the shader draws (rad)

struct Star
{
    float   dir[3];       // J2000 equatorial, unit
    uint8_t mag;          // V magnitude code: V = (mag - 1) / 25 - 2
    uint8_t rgb[3];
};

// The cube face and cell of unit direction e (the shader's nsCell, same float arithmetic).
inline void cell(const float e[3], int& face, int& cx, int& cy)
{
    const float ax = std::fabs(e[0]), ay = std::fabs(e[1]), az = std::fabs(e[2]);
    const int axis = (ax >= ay && ax >= az) ? 0 : (ay >= az ? 1 : 2);
    const float major = e[axis];
    const float u = (axis == 0 ? e[1] : e[0]) / std::fabs(major);
    const float v = (axis == 2 ? e[1] : e[2]) / std::fabs(major);
    face = axis * 2 + (major < 0.f ? 1 : 0);
    cx = (int)std::fmin(std::floor((u * 0.5f + 0.5f) * N), (float)(N - 1));
    cy = (int)std::fmin(std::floor((v * 0.5f + 0.5f) * N), (float)(N - 1));
}

// Octahedral encoding of a unit vector, 16 bits a component.
inline void octEncode(const float e[3], uint16_t& u, uint16_t& v)
{
    const float l1 = std::fabs(e[0]) + std::fabs(e[1]) + std::fabs(e[2]);
    float x = e[0] / l1, y = e[1] / l1;
    if (e[2] < 0.f)
    {
        const float ox = x, oy = y;
        x = (1.f - std::fabs(oy)) * (ox >= 0.f ? 1.f : -1.f);
        y = (1.f - std::fabs(ox)) * (oy >= 0.f ? 1.f : -1.f);
    }
    u = (uint16_t)std::lround((x * 0.5f + 0.5f) * 65535.f);
    v = (uint16_t)std::lround((y * 0.5f + 0.5f) * 65535.f);
}

inline void octDecode(uint16_t u, uint16_t v, float e[3])
{
    float x = u / 65535.f * 2.f - 1.f, y = v / 65535.f * 2.f - 1.f;
    const float z = 1.f - std::fabs(x) - std::fabs(y);
    if (z < 0.f)
    {
        const float ox = x, oy = y;
        x = (1.f - std::fabs(oy)) * (ox >= 0.f ? 1.f : -1.f);
        y = (1.f - std::fabs(ox)) * (oy >= 0.f ? 1.f : -1.f);
    }
    const float l = std::sqrt(x * x + y * y + z * z);
    e[0] = x / l; e[1] = y / l; e[2] = z / l;
}

// The catalogue file's stars, brightest first; false when it is not a catalogue file.
inline bool parse(const std::vector<uint8_t>& f, std::vector<Star>& out)
{
    out.clear();
    if (f.size() < 12 || std::memcmp(f.data(), "GMDXBSC1", 8) != 0) return false;
    uint32_t n = 0;
    std::memcpy(&n, &f[8], 4);
    if (n == 0 || n > 100000 || f.size() != 12 + (size_t)n * 8) return false;
    out.resize(n);
    for (uint32_t i = 0; i < n; ++i)
    {
        const uint8_t* r = &f[12 + (size_t)i * 8];
        uint16_t ra_q; int16_t dec_q;
        std::memcpy(&ra_q, r, 2);
        std::memcpy(&dec_q, r + 2, 2);
        const double ra = ra_q / 65536.0 * 2.0 * 3.14159265358979;
        const double dec = dec_q / 32767.0 * 0.5 * 3.14159265358979;
        Star& s = out[i];
        s.dir[0] = (float)(std::cos(dec) * std::cos(ra));
        s.dir[1] = (float)(std::cos(dec) * std::sin(ra));
        s.dir[2] = (float)std::sin(dec);
        s.mag = r[4] ? r[4] : 1;
        std::memcpy(s.rgb, r + 5, 3);
    }
    return true;
}

// The grid texture (W x H x 4 bytes). `dropped`: listings a full cell refused (faint stars in
// the densest clusters).
inline std::vector<uint8_t> build(const std::vector<Star>& stars, uint32_t* dropped = nullptr)
{
    std::vector<uint8_t> tex((size_t)W * H * 4, 0);
    std::vector<uint8_t> fill((size_t)6 * N * N, 0);
    uint32_t drop = 0;
    const float t = std::tan(SPLAT);
    for (const Star& s : stars)
    {
        // The cells within SPLAT: the star's own, and those under two rings of points around it,
        // 32 at 1.15 SPLAT and 16 at 0.6. A cell (>= 0.8 deg) that reaches into the SPLAT disc
        // cannot fit inside the outer ring, so it crosses it along >= 14 deg of arc (a corner just
        // poking in); the points are 11.25 deg apart.
        const float* e = s.dir;
        float a[3] = {0.f, 0.f, 1.f};
        if (std::fabs(e[2]) > 0.9f) { a[0] = 1.f; a[2] = 0.f; }
        float tu[3] = {e[1] * a[2] - e[2] * a[1], e[2] * a[0] - e[0] * a[2], e[0] * a[1] - e[1] * a[0]};
        const float tl = std::sqrt(tu[0] * tu[0] + tu[1] * tu[1] + tu[2] * tu[2]);
        for (float& c : tu) c /= tl;
        const float tv[3] = {e[1] * tu[2] - e[2] * tu[1], e[2] * tu[0] - e[0] * tu[2], e[0] * tu[1] - e[1] * tu[0]};
        int seen[49];
        int nseen = 0;
        for (int p = 0; p < 49; ++p)
        {
            const float r = p == 0 ? 0.f : (p <= 32 ? 1.15f * t : 0.6f * t);
            const float ang = p == 0 ? 0.f : (p <= 32 ? (p - 1) / 32.f : (p - 33) / 16.f) * 6.2831853f;
            float q[3];
            for (int c = 0; c < 3; ++c) q[c] = e[c] + r * (std::cos(ang) * tu[c] + std::sin(ang) * tv[c]);
            const float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
            for (float& c : q) c /= ql;
            int f, cx, cy;
            cell(q, f, cx, cy);
            const int id = (f * N + cy) * N + cx;
            bool dup = false;
            for (int k = 0; k < nseen; ++k) dup |= seen[k] == id;
            if (dup) continue;
            seen[nseen++] = id;
            if (fill[id] >= K) { ++drop; continue; }
            const int k = fill[id]++;
            uint8_t* px = &tex[(((size_t)(f * N + cy) * W) + (size_t)(cx * K + k) * 2) * 4];
            uint16_t u, v;
            octEncode(e, u, v);
            px[0] = (uint8_t)(u >> 8); px[1] = (uint8_t)(u & 255);
            px[2] = (uint8_t)(v >> 8); px[3] = (uint8_t)(v & 255);
            px[4] = s.rgb[0]; px[5] = s.rgb[1]; px[6] = s.rgb[2]; px[7] = s.mag;
        }
    }
    if (dropped) *dropped = drop;
    return tex;
}
} // namespace GMDXStarGrid

#endif // GMDX_NIGHTSKYGRID_H
