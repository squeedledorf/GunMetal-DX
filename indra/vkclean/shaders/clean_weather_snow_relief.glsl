// vkclean — SNOW SURFACE RELIEF (#906; shared by the snow coating clean_weather_surface.glsl
// weatherSnowLayer, the snow mesh's G-buffer FS clean_weather_snowmesh.frag and its forward
// variant clean_draw.frag -DCLEAN_WEATHER_SNOWMESH, so the mesh and the coated surfaces around
// it shade as one blanket).
//
// Before: the slope of ONE octave of lattice VALUE noise at a 17 cm cell, tilting the normal by
// up to ~0.18. Value noise has its gradient concentrated on a regular lattice, and under a low
// sun (sunset, sun shadows off: nothing else darkens the snow) a tilt of that size swings N.L
// across the terminator -- a regular grid of dark dashes over every snowfield (#906 "stipple").
// Now: three rotated octaves of GRADIENT noise (no lattice plateaus) with an analytic gradient,
// metres-scale heights (a few mm of relief: total tilt ~0.02 typical, < 0.07 worst), each
// octave faded out once its cells shrink under ~3 pixels (the footprint the caller passes:
// derivatives where the caller can take them, else the distance / grazing estimate below), so
// distant snow is smooth instead of aliasing. SPARKLE: sparse ~1.7 cm facets (2 %) get a
// random tilt and a glossy roughness -- point glints that follow the view and the light, kept
// only while a facet covers a couple of pixels (no sub-pixel shimmer).

#ifndef CLEAN_WEATHER_SNOW_RELIEF_GLSL
#define CLEAN_WEATHER_SNOW_RELIEF_GLSL

uint wxrHash(ivec2 c, uint salt)
{
    uint h = uint(c.x) * 0x8DA6B343u ^ uint(c.y) * 0xD8163841u ^ salt;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h;
}
vec2 wxrGrad(ivec2 c, uint salt)
{
    const float a = 6.2831853 * float(wxrHash(c, salt) & 0xFFFFu) * (1.0 / 65535.0);
    return vec2(cos(a), sin(a));
}
// Gradient noise (quintic fade): x the value (~ -0.7 .. 0.7), yz its gradient d/dp.
vec3 wxrGradNoise(vec2 p, uint salt)
{
    const vec2 fl = floor(p), f = p - fl;
    const ivec2 i = ivec2(fl);
    const vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
    const vec2 du = 30.0 * f * f * (f * (f - 2.0) + 1.0);
    const vec2 ga = wxrGrad(i, salt), gb = wxrGrad(i + ivec2(1, 0), salt);
    const vec2 gc = wxrGrad(i + ivec2(0, 1), salt), gd = wxrGrad(i + ivec2(1, 1), salt);
    const float va = dot(ga, f), vb = dot(gb, f - vec2(1.0, 0.0));
    const float vc = dot(gc, f - vec2(0.0, 1.0)), vd = dot(gd, f - vec2(1.0, 1.0));
    const float k = va - vb - vc + vd;
    const float v = va + u.x * (vb - va) + u.y * (vc - va) + u.x * u.y * k;
    const vec2 g = ga + u.x * (gb - ga) + u.y * (gc - ga) + u.x * u.y * (ga - gb - gc + gd)
                 + du * (u.yx * k + vec2(vb, vc) - va);
    return vec3(v, g);
}

// The pixel footprint (m) on a surface at distance dist whose normal makes cosV with the view
// (no derivatives: ~1.3 mrad per pixel, a 1080p view at ~70 degrees; grazing stretches it).
float wxSnowFootprint(float dist, float cosV) { return dist * 1.3e-3 / max(abs(cosV), 0.2); }

// The relief normal over geometric normal Ng at noise domain Q (metres: world xy, or a moving
// object's own frame), footprint px (m per pixel). glint 0..1: a sparkle facet here.
vec3 wxSnowRelief(vec3 Ng, vec2 Q, float px, out float glint)
{
    // octaves: frequency (1 / m), height amplitude (m), rotation (cos, sin), salt
    const vec4 kO[3] = vec4[3](vec4(1.7, 0.008, 1.0, 0.0), vec4(4.3, 0.0025, 0.80, 0.60),
                               vec4(10.9, 0.0008, -0.28, 0.96));
    vec2 slope = vec2(0.0);
    for (int k = 0; k < 3; ++k)
    {
        const float f = kO[k].x;
        const float fade = 1.0 - smoothstep(0.12, 0.33, px * f);   // (cells under ~3 pixels: gone)
        if (fade <= 0.0) continue;
        const mat2 R = mat2(kO[k].z, kO[k].w, -kO[k].w, kO[k].z);
        const vec3 n = wxrGradNoise(R * (Q * f) + vec2(17.3 * float(k), -5.1 * float(k)), 307u + uint(k) * 131u);
        slope += fade * kO[k].y * f * (transpose(R) * n.yz);   // (chain rule: d/dQ)
    }
    vec3 N = normalize(Ng - vec3(slope, 0.0));
    // SPARKLE: ~1.7 cm facets, 2 % of them, while one covers more than ~2 pixels.
    glint = 0.0;
    const float cell = 60.0;
    const float gv = 1.0 - smoothstep(0.35, 0.6, px * cell);
    if (gv > 0.0)
    {
        const ivec2 c = ivec2(floor(Q * cell));
        const uint h = wxrHash(c, 911u);
        if ((h & 0xFFu) < 5u)
        {
            const float a = 6.2831853 * float((h >> 8) & 0xFFFu) * (1.0 / 4095.0);
            const float t = 0.15 + 0.25 * float((h >> 20) & 0xFFu) * (1.0 / 255.0);
            N = normalize(N + vec3(cos(a), sin(a), 0.0) * t);
            glint = gv;
        }
    }
    return N;
}

#endif   // CLEAN_WEATHER_SNOW_RELIEF_GLSL
