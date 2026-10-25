// vkclean — DEFERRED G-BUFFER layout (RenderVKDeferredLighting): pack / unpack of a
// SurfaceLit (clean_surface_light.glsl). Written by clean_draw.frag -DCLEAN_GBUFFER (pass
// 1b's G-buffer draws, EQUAL after the depth pre-pass), read by
// clean_deferred_light.comp. Depth is the pre-pass depth (reconstructed position).
//
//   G0 RGBA8_SRGB          rgb albedo (linear in, sRGB-encoded storage), a = PBR ORM
//                          occlusion | legacy env intensity | fullbright shiny level
//   G1 RGBA16F             xy oct(N) shading normal, zw oct(Ng) geometric normal
//   G2 RGBA8_UNORM         x PBR roughness | legacy glossiness, y PBR metallic | <GMDX>
//                          legacy emissive-mask weight (SurfaceLit.emask, 0 otherwise),
//                          z glow (LL te glow, PBR: x max emissive), w model byte:
//                          bits 0-1 SURF_* (0 = no surface: cleared), bit 2 no distance fade,
//                          bit 3 cel shading (#550, SurfaceLit.cel), bit 4 <GMDX> rez effect
//                          emission in G3 (legacy: instead of the specular colour); bits 5-7 free
//   G3 B10G11R11_UFLOAT    PBR emissive (linear HDR) | legacy specular colour (linear)
// 20 B / pixel (+ the existing depth).
//
// GLOW: the G-buffer carries the glow FACTOR only; the lighting pass emits the glow
// contribution (display-encoded lit colour x factor) through ONE function
// (clean_deferred_light.comp writeLitOutput), the merge point for packing glow elsewhere.

#ifndef CLEAN_GBUFFER_GLSL
#define CLEAN_GBUFFER_GLSL

vec2 gbOctWrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec2 gbOctEncode(vec3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-20);
    return n.z >= 0.0 ? n.xy : gbOctWrap(n.xy);
}
vec3 gbOctDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}

const uint GB_NOFADE = 4u;
const uint GB_CEL    = 8u;   // toon lighting (#550)
const uint GB_REZFX  = 16u;  // <GMDX> legacy face with rez effect emission: G3 = emissive (no specular)

void gbufferPack(SurfaceLit s, float glow, bool noFade,
                 out vec4 g0, out vec4 g1, out vec4 g2, out vec4 g3)
{
    float a = 0.0, x = 0.0, y = 0.0;
    vec3 e = vec3(0.0);
    if (s.model == SURF_PBR)            { a = s.ao;  x = s.rough; y = s.metal; e = s.emissive; }
    else if (s.model == SURF_LEGACY)    { a = s.env; x = s.gloss; y = s.emask; e = s.spec; }   // <GMDX> y
    else if (s.model == SURF_FULLBRIGHT){ a = s.env; }
    // <GMDX> rez effect: a legacy face's emission takes G3 (its specular colour drops there)
    const bool rezfx = s.model == SURF_LEGACY && max(max(s.emissive.r, s.emissive.g), s.emissive.b) > 0.0;
    if (rezfx) e = s.emissive;
    g0 = vec4(clamp(s.albedo, vec3(0.0), vec3(1.0)), clamp(a, 0.0, 1.0));
    g1 = vec4(gbOctEncode(s.N), gbOctEncode(s.Ng));
    const uint mb = (s.model & 3u) | (noFade ? GB_NOFADE : 0u) | (s.cel ? GB_CEL : 0u) | (rezfx ? GB_REZFX : 0u);
    g2 = vec4(clamp(x, 0.0, 1.0), clamp(y, 0.0, 1.0), clamp(glow, 0.0, 1.0), float(mb) * (1.0 / 255.0));
    g3 = vec4(max(e, vec3(0.0)), 0.0);
}

SurfaceLit gbufferUnpack(vec4 g0, vec4 g1, vec4 g2, vec4 g3, out float glow, out bool noFade)
{
    SurfaceLit s = surfaceInit();
    const uint mb = uint(g2.w * 255.0 + 0.5);
    s.model = mb & 3u;
    noFade = (mb & GB_NOFADE) != 0u;
    s.cel = (mb & GB_CEL) != 0u;
    glow = g2.z;
    s.albedo = g0.rgb;
    s.N  = gbOctDecode(g1.xy);
    s.Ng = gbOctDecode(g1.zw);
    if (s.model == SURF_PBR)
    {
        s.ao = g0.a; s.rough = g2.x; s.metal = g2.y; s.emissive = g3.rgb;
    }
    else if (s.model == SURF_LEGACY)
    {
        s.env = g0.a; s.gloss = g2.x; s.spec = g3.rgb;
        s.emask = g2.y;   // <GMDX> emissive mask
        if ((mb & GB_REZFX) != 0u) { s.emissive = g3.rgb; s.spec = vec3(0.0); }   // <GMDX>
    }
    else if (s.model == SURF_FULLBRIGHT)
    {
        s.env = g0.a;
    }
    return s;
}

#endif   // CLEAN_GBUFFER_GLSL
