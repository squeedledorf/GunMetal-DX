#version 450
// <GMDX> vkclean -- SELECTION OUTLINE edge pass (GMDXSelectionStyle 1,
// vkgfx/gmdxseloutlinevk.cpp). Reads the selection mask (gmdx_sel_mask.frag) and draws, blended
// over the finished frame:
//   * the outer contour: a pixel outside every selected face that has a selected pixel within
//     the line width -- coloured by that neighbour's kind, a visible one preferred, dimmed if
//     only occluded ones are near (and dropped then unless hidden selections are shown, or it
//     is the hover preview, which always shows through);
//   * face edges: where the face id changes between two VISIBLE selected pixels, on one side
//     only (the lower key), in the pixel's own kind colour -- so a selected box shows all its
//     visible edges, as LL's silhouette does. Occluded pixels carry no reliable face (the
//     hidden pass has no depth order), so they only add to the contour.
// The occluded bit never makes an edge by itself: where a wall cuts across an object, no line.
#extension GL_GOOGLE_include_directive : require
#include "gmdx_sel_outline_common.glsl"

layout(set = 0, binding = 0) uniform usampler2D uSelMask;
layout(location = 0) out vec4 outColor;

void main()
{
    const ivec2 px = ivec2(gl_FragCoord.xy);
    const ivec2 pmax = ivec2(p.dims.zw) - 1;
    const uint c = texelFetch(uSelMask, px, 0).r;
    const uint c_key = c & ~4u;
    const bool c_vis = (c & 4u) == 0u;
    const int r = int(p.params.x);
    const int r2 = r * r + r;   // a round-ish disc
    uint best = 0u;
    for (int dy = -r; dy <= r; ++dy)
    {
        for (int dx = -r; dx <= r; ++dx)
        {
            if ((dx == 0 && dy == 0) || dx * dx + dy * dy > r2) continue;
            const uint n = texelFetch(uSelMask, clamp(px + ivec2(dx, dy), ivec2(0), pmax), 0).r;
            if (n == 0u || (n & ~4u) == c_key) continue;
            if (c != 0u)
            {
                // face / kind edge inside the selection: both sides visible, lower key draws
                if (c_vis && (n & 4u) == 0u && (n & ~4u) > c_key) best = c;
            }
            else if (best == 0u || ((best & 4u) != 0u && (n & 4u) == 0u))
            {
                best = n;   // outer contour: prefer a visible neighbour
            }
        }
    }
    if (best == 0u) discard;
    const uint kind = best & 3u;
    const bool occluded = (best & 4u) != 0u;
    if (occluded && p.params.y < 0.5 && kind != 3u) discard;
    vec4 col = p.colors[kind - 1u];
    if (occluded) col.a *= p.params.z;
    outColor = col;
}
