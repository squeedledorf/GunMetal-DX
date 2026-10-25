#version 460
#extension GL_GOOGLE_include_directive : require
// vkclean — WEATHER PRECIPITATION VEIL, the FOG-OFF fallback for opaque + sky (#954;
// vkgfx/llcleanweathervk.cpp, clean_weather_veil.glsl). With the volumetric fog on the
// precipitation is a medium in its froxels instead and this is not drawn.
//
// A fullscreen triangle at the start of render pass 2 (the transparents veil themselves),
// blending dst' = src.rgb + dst.rgb x (1 - src.a) with src = (L (1 - T), 1 - T): T = exp(-sigma d),
// d the path to the opaque depth (draw set 0 binding 40, read only in pass 2) below the
// precipitation's ceiling, sigma / L ssr.weather[15]. Noise-free, and only where the SURFACE is
// open to the sky at its own position (the roof maps, draw set 0 binding 49): a surface under
// cover -- a room seen from a doorway, the underside of a porch -- is never veiled. Sky pixels are
// open. Glow and the reactive mask are write-masked off. The camera under water: not drawn.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 o_color;
layout(location = 1) out vec4 o_glow;    // (write-masked)
layout(location = 2) out vec4 o_react;   // (write-masked)

#include "clean_frame_info.glsl"
#include "clean_weather_veil.glsl"
#define WEATHER_VS          // (read-only access to the weather's buffers)
#define WX_OWN_SET 1        // (draw set 0 first, the weather's own set at 1)
#include "clean_weather_common.glsl"

layout(set = 0, binding = 40) uniform sampler2D uSceneDepth;

// The far end of a sky pixel's path (m) when the ray stays under the ceiling (near the horizon).
const float kVeilFar = 3000.0;

void main()
{
    o_glow = vec4(0.0);
    o_react = vec4(0.0);
    o_color = vec4(0.0);
    const float sigma = ssr.weather[15].w;
    if (!(sigma > 0.0)) return;
    const ivec2 p = ivec2(gl_FragCoord.xy);
    const float dz = texelFetch(uSceneDepth, p, 0).r;
    // (view_proj is the JITTERED camera-relative matrix the scene was drawn with: no jitter to undo)
    const vec2 ndc = (vec2(p) + 0.5) * wp.screen.zw * 2.0 - 1.0;
    const bool rev = wp.misc.w > 0.5;
    const bool sky = rev ? !(dz > 0.0) : !(dz < 1.0);
    vec3 rel;
    if (!sky)
    {
        const vec4 w = wp.inv_view_proj * vec4(ndc, dz, 1.0);
        rel = w.xyz / w.w;
        // A surface under cover is never veiled.
        if (weatherPrecipOpen(wp.eye.xyz + rel) < 0.5) return;
    }
    else
    {
        const vec4 w = wp.inv_view_proj * vec4(ndc, 0.5, 1.0);
        rel = normalize(w.xyz / w.w) * kVeilFar;
    }
    const float len = max(length(rel), 1e-3);
    float d = len;
    if (rel.z > 0.0) d = min(d, kWxVeilCeiling * len / rel.z);
    const float a = clamp(1.0 - exp(-sigma * d), 0.0, 1.0);
    o_color = vec4(ssr.weather[15].rgb * a, a);
}
