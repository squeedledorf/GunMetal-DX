#version 450
#extension GL_GOOGLE_include_directive : require
#include "clean_av_slots.glsl"
// vkclean — CEL OUTLINES (#550, llcelshadevk.cpp). Fullscreen triangle
// (clean_glow_combine.vert) blended (src-alpha over) into the RENDER-res scene HDR after
// pass 1 and the avatar-id pass, before the god rays / pass 2 -- so TAA anti-aliases the
// lines and alpha / water draw over them. Main view + offscreen snapshots only.
//
// Edges, tested against 8 taps at the line radius (cross + diagonals):
//   * avatar-id discontinuity (silhouette vs world, avatar vs avatar),
//   * linear-depth discontinuity (relative; self-occlusion, an arm over the torso),
//   * creases: the linear-depth second difference across the pixel (a slope change =
//     a normal discontinuity, without needing the normal target on every frame).
// The line goes on the FARTHER side of a depth / id edge (the near surface keeps its
// shape; in avatars-only mode the outer silhouette lands on the world pixels around
// the avatar -- the dilated mask). Avatars-only: an edge counts only when one side is a
// selected avatar (depth / crease edges: inside a selected avatar).
// Fades out with distance (fade start / end, metres). The colour is linear HDR, scaled on
// the CPU by RenderVKCelOutlineIntensity (black lines need nothing; a coloured line in a
// very bright / dark exposure may want the intensity adjusted -- follow-up: read the
// dynamic exposure).
layout(set = 0, binding = 0) uniform usampler2D uId;     // render-res avatar slot + 1
layout(set = 0, binding = 1) uniform sampler2D  uDepth;  // world depth (reversed-Z)
layout(std430, set = 0, binding = 2) readonly buffer CelParamsSSBO {
    uint  cel_flags;
    uint  cel_bands;
    float cel_softness;
    float cel_flatten;
    vec4  cel_shadow;
    vec4  cel_rim;
    vec4  cel_spec;
    uint  cel_sel[AV_SLOT_WORDS];
};

layout(push_constant) uniform Push {
    vec4 dims;     // render w, h, 1/w, 1/h
    vec4 lin;      // view depth = |(x d + y) / (z d + w)| (inverse projection terms)
    vec4 color;    // rgb line colour (linear), a opacity
    vec4 p0;       // x radius (render px), y depth threshold (relative), z crease threshold, w mode (0 scene, 1 avatars)
    vec4 p1;       // x fade start (m), y fade end (m)
} pc;

layout(location = 0) out vec4 outColor;

const uint CEL_ALL = 4u;

bool selected(uint id)
{
    if (id == 0u) return false;
    if ((cel_flags & CEL_ALL) != 0u) return true;
    const uint slot = id - 1u;
    return slot < AV_SLOT_CAP && ((cel_sel[slot >> 5u] >> (slot & 31u)) & 1u) != 0u;
}

float linearDepth(float d) { return abs((pc.lin.x * d + pc.lin.y) / (pc.lin.z * d + pc.lin.w)); }

void main()
{
    const ivec2 p = ivec2(gl_FragCoord.xy);
    const ivec2 lim = ivec2(pc.dims.xy) - 1;
    const bool sceneMode = pc.p0.w < 0.5;
    const int r = max(int(pc.p0.x + 0.5), 1);
    const ivec2 offs[8] = ivec2[8](ivec2(r, 0), ivec2(-r, 0), ivec2(0, r), ivec2(0, -r),
                                   ivec2(r, r), ivec2(-r, -r), ivec2(r, -r), ivec2(-r, r));

    const uint id0 = texelFetch(uId, p, 0).r;
    const bool sel0 = selected(id0);
    uint ids[8];
    bool anySel = sel0;
    for (int i = 0; i < 8; ++i)
    {
        ids[i] = texelFetch(uId, clamp(p + offs[i], ivec2(0), lim), 0).r;
        anySel = anySel || selected(ids[i]);
    }
    // Avatars only: nothing selected around this pixel -> no line (no depth fetches).
    if (!sceneMode && !anySel) discard;

    const float d0 = texelFetch(uDepth, p, 0).r;
    const float z0 = d0 <= 0.0 ? 1e6 : linearDepth(d0);   // reversed-Z: 0 = sky
    float edge = 0.0;
    float zLine = z0;
    float z[8];
    for (int i = 0; i < 8; ++i)
    {
        const float d = texelFetch(uDepth, clamp(p + offs[i], ivec2(0), lim), 0).r;
        z[i] = d <= 0.0 ? 1e6 : linearDepth(d);
        const bool idEdge = ids[i] != id0 && (sceneMode || sel0 || selected(ids[i]));
        // The neighbour is the nearer side (ties: the higher id draws, so one side only).
        const bool nearer = z[i] < z0 * (1.0 - 1e-4) || (abs(z[i] - z0) <= z0 * 1e-4 && ids[i] > id0);
        if (idEdge && nearer) { edge = 1.0; zLine = min(zLine, z[i]); }
        const bool depthEdge = (z0 - z[i]) > pc.p0.y * z[i] && (sceneMode || (sel0 && ids[i] == id0));
        if (depthEdge) { edge = 1.0; zLine = min(zLine, z[i]); }
    }
    // Creases: second difference along the 4 axes through the pixel (inside a surface).
    if (pc.p0.z > 0.0 && (sceneMode || sel0) && z0 < 1e5)
    {
        for (int a = 0; a < 8; a += 2)
        {
            if (ids[a] != id0 || ids[a + 1] != id0 || z[a] >= 1e5 || z[a + 1] >= 1e5) continue;
            const float c = abs(z[a] + z[a + 1] - 2.0 * z0);
            if (c > pc.p0.z * z0) edge = max(edge, 0.75);
        }
    }
    if (edge <= 0.0) discard;

    float a = edge * pc.color.a;
    if (pc.p1.y > pc.p1.x) a *= 1.0 - smoothstep(pc.p1.x, pc.p1.y, zLine);
    if (a <= 0.004) discard;
    outColor = vec4(pc.color.rgb, a);
}
