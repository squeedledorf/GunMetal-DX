#version 460
#extension GL_GOOGLE_include_directive : require
// vkclean — WEATHER PRECIPITATION DRAW, vertex stage (#783; vkgfx/llcleanweathervk.cpp): one
// instance per visible particle (clean_weather_sim.comp's list; indirect instance count),
// 6 vertices = one quad as two triangles, built in screen space:
//   rain  a streak from the drop to where it was shape.x seconds ago (the motion blur of a
//         camera exposure), shape.y metres wide;
//   snow  a round flake, shape.z metres across.
// Both keep a minimum on-screen width (1 px streak / 1.5 px flake) and trade the missing
// width for opacity (coverage preserving), so distant drops thin out instead of
// shimmering. They fade toward the box edges, where the wrap would pop them.
// #954: a drop's width and streak follow its size and fall speed (bigger, faster drops in heavy
// rain); a flake's size its clumping share, and a flake whose motion over the snow exposure is
// longer than itself is drawn MOTION-BLURRED along it (wind-driven / blizzard snow streaks);
// blowing snow (a thinner, ground-hugging share) likewise. Every particle takes the veil of its
// own distance (v_veil: the precipitation's in-scatter in front of it, clean_weather.frag).
#define WEATHER_VS
#include "clean_weather_common.glsl"

layout(location = 0) out vec2 v_uv;          // rain: x along (0 head .. 1 tail), y across (-1..1); snow: -1..1
layout(location = 1) flat out float v_alpha;
layout(location = 2) flat out float v_snow;
layout(location = 4) flat out float v_veil;   // #954: the veil's transmittance to the particle (1 = none)
#ifdef WEATHER_STREAM
// M4 RUN-OFF STREAMS (-DWEATHER_STREAM): one instance per run-off emitter (the list's indirect
// draw); an emitter whose discharge (catchment x strength) passes the stream threshold is a thin
// ribbon from its edge down to where it lands (camera-facing about its axis, drifted by the
// wind over its fall), fading in over the threshold's first 60 % (no pop from drips to a
// stream); the others collapse. v_uv x along (0 edge .. 1 landing), y across; v_len its
// length (m); v_snow 3 + a per-emitter seed.
layout(location = 3) flat out float v_len;
#endif
#ifdef WEATHER_SPLASH
// M2 SPLASHES (-DWEATHER_SPLASH): one instance per ring slot -- a small upright spray quad
// standing on the hit (camera-facing about the vertical), alive for wp.splash.z seconds;
// v_uv x across (-1..1), y up (0..1); v_age 0..1 of its life; v_snow 2 + a per-splash seed.
layout(location = 3) flat out float v_age;
const float kSplashWidth = 0.09;    // m
const float kSplashHeight = 0.07;   // m (at its tallest)
const float kSplashOpacity = 0.5;
#endif

const vec2 kCorner[6] = vec2[6](vec2(0.0, -1.0), vec2(1.0, -1.0), vec2(1.0, 1.0),
                                vec2(0.0, -1.0), vec2(1.0, 1.0), vec2(0.0, 1.0));

#if defined(WEATHER_STREAM)
void main()
{
    const vec4 e0 = emitters[2u * uint(gl_InstanceIndex)], e1 = emitters[2u * uint(gl_InstanceIndex) + 1u];
    const vec2 cr = kCorner[gl_VertexIndex % 6];
    v_uv = cr;
    v_alpha = 0.0;
    v_snow = 3.0 + fract(float(gl_InstanceIndex) * 0.61803399);
    v_len = 0.0;
    v_veil = 1.0;
    gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
    const float q = e0.w * wp.runoff.x;
    const float th = max(wp.runoff.z, 1e-3);
    if (wp.runoff2.z < 0.5 || q < th) return;
    const float land = wxDripLand(e1);
    const float h = e0.z - land;
    if (h < 0.05) return;
    const vec2 outDir = normalize(e1.yz + vec2(1e-6));
    const vec3 top = vec3(e0.xy + outDir * 0.02, e0.z);
    const float tFall = sqrt(2.0 * h / kWxGravity);
    const vec3 bot = vec3(top.xy + wp.wind.xy * (0.5 * tFall), land);
    const vec3 a = top - wp.eye.xyz, b = bot - wp.eye.xyz;
    vec4 ca = wp.view_proj * vec4(a, 1.0), cb = wp.view_proj * vec4(b, 1.0);
    if (ca.w < 0.05 && cb.w < 0.05) return;
    // (an end behind the camera: clipped onto the near side along the ribbon)
    if (ca.w < 0.05) ca = mix(cb, ca, (cb.w - 0.05) / max(cb.w - ca.w, 1e-4));
    if (cb.w < 0.05) cb = mix(ca, cb, (ca.w - 0.05) / max(ca.w - cb.w, 1e-4));
    const vec2 halfScreen = 0.5 * wp.screen.xy;
    const float pxPerM = 0.5 * wp.screen.x * length(vec3(wp.view_proj[0][0], wp.view_proj[1][0], wp.view_proj[2][0]));
    const vec2 sa = ca.xy / ca.w * halfScreen, sb = cb.xy / cb.w * halfScreen;
    const vec2 dv = sb - sa;
    const float len = length(dv);
    const vec2 dir = len > 1e-3 ? dv / len : vec2(0.0, 1.0);
    const vec2 perp = vec2(-dir.y, dir.x);
    // Width: a thin trickle to a ~3 cm sheet with the discharge.
    const float wM = clamp(0.004 + 0.006 * sqrt(q), 0.004, 0.03);
    vec4 c = mix(ca, cb, cr.x);
    const float wpx = wM * pxPerM / c.w;
    const float hw = 0.5 * max(wpx, 1.0);
    c.xy += perp * (cr.y * hw) / halfScreen * c.w;
    v_alpha = min(wpx, 1.0) * 0.55 * smoothstep(th, th * 1.6, q);
    v_len = h;
    v_veil = wxVeilT(0.5 * (a + b));
    gl_Position = c;
}
#elif defined(WEATHER_SPLASH)
void main()
{
    const vec4 sp = splashes[gl_InstanceIndex];
    const float age = wp.eye.w - sp.w;
    const vec2 cr = kCorner[gl_VertexIndex % 6];
    v_uv = vec2(cr.y, cr.x);
    v_alpha = 0.0;
    v_snow = 2.0 + fract(float(gl_InstanceIndex) * 0.61803399);
    v_age = clamp(age / max(wp.splash.z, 1e-3), 0.0, 1.0);
    v_veil = 1.0;
    gl_Position = vec4(2.0, 2.0, 0.0, 1.0);
    if (!(sp.w > 0.0) || age < 0.0 || age > wp.splash.z) return;
    const vec3 a = sp.xyz - wp.eye.xyz;
    const vec4 ca = wp.view_proj * vec4(a, 1.0);
    if (ca.w < 0.05) return;
    const vec4 cu = wp.view_proj * vec4(a + vec3(0.0, 0.0, kSplashHeight), 1.0);
    const vec2 halfScreen = 0.5 * wp.screen.xy;
    const float pxPerM = 0.5 * wp.screen.x * length(vec3(wp.view_proj[0][0], wp.view_proj[1][0], wp.view_proj[2][0]));
    const vec2 sa = ca.xy / ca.w * halfScreen;
    const vec2 su = cu.w > 0.05 ? cu.xy / cu.w * halfScreen : sa + vec2(0.0, 1.0);
    vec2 up = su - sa;
    const float hpx = length(up);
    up = hpx > 1e-3 ? up / hpx : vec2(0.0, 1.0);
    const vec2 across = vec2(up.y, -up.x);
    const float wpx = kSplashWidth * pxPerM / ca.w;
    const float hw = 0.5 * max(wpx, 1.5);
    const float h = max(hpx, 1.5);
    vec4 c = ca;
    c.xy += (across * (cr.y * hw) + up * (cr.x * h)) / halfScreen * c.w;
    // Coverage preserving (as the streaks) and a fade toward the spawn radius.
    const float far = 1.0 - smoothstep(0.6, 1.0, length(a.xy) / max(wp.splash.w, 1.0));
    v_alpha = min(wpx / 1.5, 1.0) * kSplashOpacity * far;
    v_veil = wxVeilT(a);
    gl_Position = c;
}
#else
void main()
{
    const uint i = visible[gl_InstanceIndex];
    // M4: a run-off drip (0x80000000 | drip): its position / velocity from its age.
    const bool drip = (i & 0x80000000u) != 0u;
    vec3 a, dripV = vec3(0.0);
    if (drip)
    {
        const vec4 da = particles[uint(wp.runoff2.y + 0.5) + 2u * (i & 0x7FFFFFFFu)];
        vec3 P;
        wxDripAt(da, max(wp.eye.w - da.w, 0.0), P, dripV);
        a = P - wp.eye.xyz;
    }
    else
        a = particles[i].xyz - wp.eye.xyz;
    const bool snow = !drip && wxIsSnow(i);
    const bool blow = !drip && wxIsDrift(i);
    const vec2 cr = kCorner[gl_VertexIndex % 6];
    v_snow = snow ? 1.0 : 0.0;
    v_uv = cr;
    v_alpha = 0.0;
    v_veil = wxVeilT(a);
    const vec4 ca = wp.view_proj * vec4(a, 1.0);
    if (ca.w < 0.05)
    {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);   // (every vertex of the instance: no area)
        return;
    }
    // Pixels per metre at unit view depth: the projection's x scale (rotation-only view:
    // row 0 of view_proj has the norm of P[0][0]) x half the render width.
    const float pxPerM = 0.5 * wp.screen.x * length(vec3(wp.view_proj[0][0], wp.view_proj[1][0], wp.view_proj[2][0]));
    const vec2 halfScreen = 0.5 * wp.screen.xy;
    // Fade near the box walls (horizontal) and its top / bottom: the wrap is invisible.
    const float edge = max(max(abs(a.x), abs(a.y)) / wp.box.x, abs(a.z) / wp.box.y);
    const float fade = drip ? 1.0 : 1.0 - smoothstep(0.8, 1.0, edge);

    vec4 c = ca;
    if (!snow)
    {
        // (a drip: its own velocity over twice the exposure -- slow drips still read as streaks)
        const vec3 b = drip ? a - dripV * (2.0 * wp.shape.x) : a - wxVelocity(i, false, a + wp.eye.xyz) * wp.shape.x;
        vec4 cb = wp.view_proj * vec4(b, 1.0);
        if (cb.w < 0.05) cb = ca;   // (tail behind the camera: a dot)
        const vec2 sa = ca.xy / ca.w * halfScreen;
        const vec2 sb = cb.xy / cb.w * halfScreen;
        const vec2 dv = sb - sa;
        const float len = length(dv);
        const vec2 dir = len > 1e-3 ? dv / len : vec2(0.0, 1.0);
        const vec2 perp = vec2(-dir.y, dir.x);
        // (#954: x the drop's diameter over 2 mm -- the base width's drop)
        const float sizeF = drip ? 0.7 : clamp(wxDropDiam(i) * 0.5, 0.4, 3.0);
        const float wpx = sizeF * wp.shape.y * pxPerM / ca.w;   // true width (px)
        const float hw = 0.5 * max(wpx, 1.0);           // drawn half width
        c = mix(ca, cb, cr.x);
        const vec2 off = perp * (cr.y * hw) + dir * ((cr.x * 2.0 - 1.0) * hw);
        c.xy += off / halfScreen * c.w;
        v_alpha = min(wpx, 1.0) * wp.shape.w * fade * (drip ? 1.6 : 1.0);
    }
    else
    {
        // #954: the flake's size (its clumping share; blowing snow: fine grains), and its motion
        // over the snow exposure -- longer than the flake: a motion-blurred streak along it.
        const float sizeM = wp.shape.z * (blow ? 0.5 : wxFlakeSize(i));
        const float spx = sizeM * pxPerM / ca.w;   // true diameter (px)
        const float dpx = max(spx, 1.5);
        const vec3 V = wxVelocity(i, true, a + wp.eye.xyz) * (blow ? vec3(1.0, 1.0, 0.0) : vec3(1.0));
        vec4 cb = wp.view_proj * vec4(a - V * wp.shape2.z, 1.0);
        vec2 dir = vec2(0.0, 1.0);
        float lpx = 0.0;
        if (cb.w > 0.05)
        {
            const vec2 dv = (cb.xy / cb.w - ca.xy / ca.w) * halfScreen;
            lpx = length(dv);
            if (lpx > 1e-3) dir = dv / lpx;
        }
        // (the quad: dpx across, dpx + the motion along; the profile stays round in its own uv,
        // the coverage spread over the longer quad)
        const float along = dpx + lpx;
        const vec2 perp = vec2(-dir.y, dir.x);
        const vec2 q = vec2(cr.x * 2.0 - 1.0, cr.y);
        c.xy += (dir * (q.x * 0.5 * along + 0.5 * lpx) + perp * (q.y * 0.5 * dpx)) / halfScreen * c.w;
        v_uv = q;
        // (coverage preserving up to 4 flake lengths: a long blizzard streak stays visible)
        v_alpha = (spx * spx) / (dpx * min(along, 4.0 * dpx)) * wp.misc.z * fade * (blow ? 0.6 : 1.0);
    }
    gl_Position = c;
}
#endif   // WEATHER_STREAM / WEATHER_SPLASH
