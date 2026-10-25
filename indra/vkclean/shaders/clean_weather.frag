#version 460
#extension GL_GOOGLE_include_directive : require
// vkclean — WEATHER PRECIPITATION DRAW, fragment stage (#783): premultiplied over the scene
// (ONE, ONE_MINUS_SRC_ALPHA), depth tested against the opaque + water depth, no depth
// write. Lit cheaply from the world draws' sun / ambient: a drop mostly shows the sky it
// refracts, a flake is a white diffuser. Coverage goes to the reactive mask (TAA / FSR do
// not smear the streaks); no glow. #954: each takes the veil of its own distance (v_veil).
#define WEATHER_VS   // (read-only access to the shared buffers)
#include "clean_weather_common.glsl"

layout(location = 0) in vec2 v_uv;
layout(location = 1) flat in float v_alpha;
layout(location = 2) flat in float v_snow;
layout(location = 4) flat in float v_veil;   // #954 (clean_weather.vert)
#ifdef WEATHER_SPLASH
layout(location = 3) flat in float v_age;
#endif
#ifdef WEATHER_STREAM
layout(location = 3) flat in float v_len;
#endif

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec4 outGlow;
layout(location = 2) out vec4 outReact;

void main()
{
    float a;
    vec3 col;
#if defined(WEATHER_STREAM)
    // M4 STREAM: a thin falling sheet -- a soft profile across, the water's pulses scrolling
    // down it (per-emitter phase), fading in at the edge and at the landing.
    {
        const float seed = v_snow - 3.0;
        const float across = 1.0 - v_uv.y * v_uv.y;
        const float x = v_uv.x;
        const float s = fract(x * v_len * 2.2 - wp.eye.w * 4.5 + seed * 7.0);
        const float pulse = 0.55 + 0.45 * smoothstep(0.25, 0.75, s) * (1.0 - smoothstep(0.75, 1.0, s));
        const float ends = smoothstep(0.0, 0.04, x) * (1.0 - smoothstep(0.94, 1.0, x));
        a = v_alpha * across * pulse * ends;
        col = wp.amb.rgb * 0.85 + wp.sun.rgb * (0.3 * wp.sun.w);
    }
#elif defined(WEATHER_SPLASH)
    // M2 SPLASH: a few droplets thrown up and out from the hit on ballistic arcs (per-splash
    // seeded), fading over the splash's short life. v_uv: x across (-1..1), y up (0..1).
    {
        const float seed = v_snow - 2.0;
        const float t = v_age;
        a = 0.0;
        for (int k = 0; k < 5; ++k)
        {
            const float h1 = fract(sin((seed * 7.0 + float(k)) * 12.9898) * 43758.5453);
            const float h2 = fract(sin((seed * 3.0 + float(k)) * 78.233) * 24634.6345);
            const vec2 p = vec2((h1 * 2.0 - 1.0) * 0.9 * t, (2.6 + 1.4 * h2) * t - 3.4 * t * t);
            const vec2 q = v_uv - vec2(p.x, max(p.y, 0.0));
            a += exp(-dot(q, q) * 90.0);
        }
        a = min(a, 1.0) * v_alpha * (1.0 - t * t);
        col = wp.amb.rgb * 0.8 + wp.sun.rgb * (0.25 * wp.sun.w);
    }
#else
    if (v_snow > 0.5)
    {
        const float r2 = dot(v_uv, v_uv);
        a = v_alpha * (1.0 - smoothstep(0.2, 1.0, r2));
        col = wp.amb.rgb * 0.9 + wp.sun.rgb * (0.6 * wp.sun.w);
    }
    else
    {
        const float across = 1.0 - v_uv.y * v_uv.y;
        const float along = smoothstep(0.0, 0.2, v_uv.x) * (1.0 - smoothstep(0.55, 1.0, v_uv.x));
        a = v_alpha * across * along;
        col = wp.amb.rgb * 0.75 + wp.sun.rgb * (0.2 * wp.sun.w);
    }
#endif
    if (a < 1.0 / 512.0) discard;
    // #954 VEIL: the precipitation between the camera and the particle (the scene behind it was
    // veiled over its whole path: drawn over that, this is exactly the veil of the particle's own
    // distance -- clean_weather_veil.frag).
    col = col * v_veil + wp.veil.rgb * (1.0 - v_veil);
    outColor = vec4(col * a, a);
    outGlow = vec4(0.0);
    outReact = vec4(min(a * 2.0, 1.0), 0.0, 0.0, 0.0);
}
