// Check of the night sky's star grid (indra/newview/gmdxnightskygrid.h) against the bundled
// catalogue, looking stars up the way vkclean/shaders/gmdx_night_sky.glsl does.
//   g++ -std=c++17 -O2 -I indra/newview scripts/nightsky/gmdx_star_grid_test.cpp -o /tmp/star_grid_test
//   /tmp/star_grid_test indra/newview/app_settings/gmdx_bsc5.bin
#include "gmdxnightskygrid.h"

#include <cassert>
#include <cstdio>
#include <fstream>
#include <iterator>

using namespace GMDXStarGrid;

int main(int argc, char** argv)
{
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<Star> stars;
    assert(parse(file, stars) && stars.size() > 9000);
    uint32_t dropped = 0;
    const std::vector<uint8_t> tex = build(stars, &dropped);

    // Every star, seen from its centre and from 96 points around it inside the splat radius:
    // the cell there lists it (decoded within 0.005 deg), unless a crowded cell dropped it.
    uint32_t misses = 0;
    double worst_err = 0.0;
    for (const Star& s : stars)
    {
        for (int p = 0; p < 97; ++p)
        {
            const float r = p == 0 ? 0.f : SPLAT * (p <= 48 ? 0.995f : 0.7f);
            const float a = p * 0.1309f;
            // offset along a tangent basis (the builder's own construction is not reused)
            const float* e = s.dir;
            const float h[3] = {std::fabs(e[0]) < 0.6f ? 1.f : 0.f, std::fabs(e[0]) < 0.6f ? 0.f : 1.f, 0.f};
            float tu[3] = {e[1] * h[2] - e[2] * h[1], e[2] * h[0] - e[0] * h[2], e[0] * h[1] - e[1] * h[0]};
            const float tl = std::sqrt(tu[0] * tu[0] + tu[1] * tu[1] + tu[2] * tu[2]);
            for (float& c : tu) c /= tl;
            const float tv[3] = {e[1] * tu[2] - e[2] * tu[1], e[2] * tu[0] - e[0] * tu[2], e[0] * tu[1] - e[1] * tu[0]};
            float q[3];
            for (int c = 0; c < 3; ++c) q[c] = e[c] + std::tan(r) * (std::cos(a) * tu[c] + std::sin(a) * tv[c]);
            const float ql = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
            for (float& c : q) c /= ql;
            int f, cx, cy;
            cell(q, f, cx, cy);
            bool found = false;
            for (int k = 0; k < K && !found; ++k)
            {
                const uint8_t* px = &tex[(((size_t)(f * N + cy) * W) + (size_t)(cx * K + k) * 2) * 4];
                if (px[7] == 0) break;
                float d[3];
                octDecode((uint16_t)(px[0] << 8 | px[1]), (uint16_t)(px[2] << 8 | px[3]), d);
                const double err = std::hypot(std::hypot((double)d[0] - s.dir[0], (double)d[1] - s.dir[1]), (double)d[2] - s.dir[2]);   // chord = angle here
                if (err < 0.005 * 3.14159265 / 180.0 && px[7] == s.mag)
                {
                    found = true;
                    worst_err = std::fmax(worst_err, err);
                }
            }
            if (!found)
            {
                ++misses;
                if (s.mag <= 1 + (5.0 + 2.0) * 25) std::fprintf(stderr, "missed V %.2f at %d dir %f %f %f\n", (s.mag - 1) / 25.0 - 2.0, p, s.dir[0], s.dir[1], s.dir[2]);
                assert(s.mag > 1 + (5.0 + 2.0) * 25);   // only stars fainter than V 5 may be dropped
            }
        }
    }
    std::printf("%zu stars, %u listings dropped, %u lookups missed (all fainter than V 5), worst position error %.5f deg\n",
                stars.size(), dropped, misses, worst_err * 180.0 / 3.14159265);
    assert(misses <= dropped * 97);
    return 0;
}
