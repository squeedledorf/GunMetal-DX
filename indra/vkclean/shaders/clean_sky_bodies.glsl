// vkclean — the textured parts of the WindLight / EEP sky, per pixel of the sky pass:
//   skySceneryPre()      <- skyF.glsl: haze + rainbow + 22-degree halo, doubled/clamped
//   skyBodiesComposite() <- LLDrawPoolWLSky::renderHeavenlyBodies (sunDiscF, moonF on
//                           LLVOSky's billboards) + renderStarsDeferred (starsV/F on
//                           LLVOWLSky's star quads)
//
// LL's order in the sky gbuffer (the space skyColorPre / cloudComposite work in):
// haze (clamp(color*2, 0, 5)) -> sun (alpha blend) -> moon (alpha blend, writes depth
// where its alpha > 2/255) -> stars (additive rgb*a, depth-tested behind the whole sun
// quad and the opaque moon) -> clouds (clean_clouds.glsl). Texels are sampled raw
// (UNORM), as LL does before the SKIP_ATMOS resolve.
//
// Billboards: LL's quad sits at to_dir * HEAVENLY_BODY_DIST with half extents scaled by
// the same distance, so it is the tangent plane of the unit sphere at to_dir; the view
// ray is intersected with it and the corner texcoords (-right+up -> (0,1), +right-up
// -> (1,0)) interpolated. The basis (incl. LL's zenith special case) comes from the
// CPU. Texture rows are uploaded to Vulkan in LLImageRaw order exactly as GL gets them,
// so LL's texcoords carry over unflipped.
//
// Stars: LL scatters 1000 random quads over the upper hemisphere. There is no list to
// port, so the same distribution is drawn procedurally: the star-space direction picks a
// cube-face grid cell; a hash decides whether the cell holds a star and where (inset so
// the whole quad stays in its cell -- one cell test per pixel), its quad size (16..36
// units at the dome radius) and tint (r, b in .75..1). RenderStarCount scales the count
// (sky_fx2.x = count / upstream default; 1000 at the default).
//
// Requires: clean_frame_info.glsl (ssr.bodies / atmos / sky_fx2), clean_atmos_common.glsl,
// and the bindless table `uTextures` (set 1, binding 0) with GL_EXT_nonuniform_qualifier.
// Layout of ssr.bodies (LLVKWorldFrame::bodies, filled in llappviewer.cpp):
//   [0] sun dir.xyz (world, unit), drawn (sun up)   [1] sun right * half width, A/B blend
//   [2] sun up * half height, 0                     [3..5] moon, same ([4].w unused)
//   [6] moisture level, droplet radius, ice level, 0
//   [7] star alpha (star_brightness / 500), field rotation (rad about Z), twinkle time, 0
//   [8] tex idx sun A, sun B, moon, bloom           [9] tex idx rainbow, halo, -, -
// <GMDX> The night sky (gmdx_night_sky.glsl) takes the spare .w of [2], [4] - [7], [9].zw,
// sky_fx2.yzw and the top 24 slots of ssr.meteors (NS_SPACE); all zero while GMDXPhysicalNight
// is off.

const float SKY_STAR_RADIUS = 15000.0;   // LLVOWLSky DISTANCE_TO_STARS (dome radius)
const float SKY_STAR_GRID   = 32.0;      // cells per cube-face edge
const float SKY_STAR_COUNT  = 1000.0;    // LLVOWLSky::getStarsNumVerts

vec4 skyTex(float idx, vec2 uv)
{
    return texture(uTextures[nonuniformEXT(uint(int(idx)))], uv);
}

// LL sets TAM_CLAMP on the sun/moon/bloom textures; the bindless sampler repeats, so keep
// the bilinear taps inside. Explicit gradients: callers sample in divergent branches.
vec4 skyTexClampGrad(float idx, vec2 uv, vec2 dx, vec2 dy)
{
    uint i = uint(int(idx));
    vec2 h = 0.5 / vec2(textureSize(uTextures[nonuniformEXT(i)], 0));
    return textureGrad(uTextures[nonuniformEXT(i)], clamp(uv, h, vec2(1.0) - h), dx, dy);
}

// ---------------- skyF: rainbow + 22-degree halo ----------------
vec3 skyRainbow(float d)
{
    float moisture = ssr.bodies[6].x;
    if (moisture <= 0.0 || ssr.bodies[9].x < 0.0) return vec3(0.0);
    // SL-13629: texture inverted, interesting part kept in 0.175..0.25, interior stretched
    // over the rest (the map is sampled with REPEAT, as LL's).
    d = clamp(-0.575 - d, 0.0, 1.0);
    float interior_coord = max(0.0, d - 0.25) * 4.2857;
    d = clamp(d, 0.0, 0.25) + interior_coord;
    float rad = (ssr.bodies[6].y - 5.0) / 1024.0;
    return pow(skyTex(ssr.bodies[9].x, vec2(rad + 0.5, d)).rgb, vec3(1.8)) * moisture;
}

vec3 skyHalo22(float d)
{
    float ice = ssr.bodies[6].z;
    if (ice <= 0.0 || ssr.bodies[9].y < 0.0) return vec3(0.0);
    d = clamp(d, 0.1, 1.0);
    float v = sqrt(clamp(1.0 - d * d, 0.0, 1.0));
    return skyTex(ssr.bodies[9].y, vec2(0.0, v)).rgb * ice;
}

// LL's sky gbuffer value (skyF): haze + rainbow + halo, doubled and clamped.
vec3 skySceneryPre(vec3 dir_world)
{
    float optic_d;
    vec3 color = skyHazePre(dir_world, optic_d);
    color += skyRainbow(optic_d);
    color += skyHalo22(optic_d);
    return clamp(color * 2.0, vec3(0), vec3(5));
}

// ---------------- heavenly bodies ----------------
// CLEAN_SKY_SCENERY_ONLY: compute consumers (the ray-traced probe capture's sky on a
// miss, clean_probe_rt.comp) take skySceneryPre alone -- the bodies and stars below
// take screen-space derivatives, which only fragment shaders have. (LL skips the
// discs / stars in cube snapshots anyway.)
#ifndef CLEAN_SKY_SCENERY_ONLY
// Billboard coords of view dir d on body k's quad: (x, y) in [-1, 1] inside.
vec2 skyBillboard(vec3 d, int k)
{
    vec3 c = ssr.bodies[k].xyz;
    vec3 r = ssr.bodies[k + 1].xyz;
    vec3 u = ssr.bodies[k + 2].xyz;
    float cd = dot(d, c);
    vec3 off = d / max(cd, 1e-4) - c;
    vec2 xy = vec2(dot(off, r) / max(dot(r, r), 1e-12), dot(off, u) / max(dot(u, u), 1e-12));
    return cd > 1e-4 ? xy : vec2(2.0);
}

bool skyInQuad(vec2 xy) { return all(lessThanEqual(abs(xy), vec2(1.0))); }

// Procedural discs, kept only while a body has no texture (none set, or not loaded yet).
// Ported from the old skyColor, which added them before LL's *2/clamp.
vec3 skyProceduralSun(vec3 pre, vec3 d)
{
    vec3  u_sun_dir  = atmosToYUp(ssr.atmos[10].xyz);
    float u_sun_size = ssr.atmos[10].w;
    float sun_gate = smoothstep(-0.05, 0.02, u_sun_dir.y);
    if (sun_gate <= 0.0 || u_sun_size <= 0.0) return pre;
    float cos_a = dot(d, normalize(u_sun_dir));
    float ang = sqrt(max(0.0, 2.0 - 2.0 * min(cos_a, 1.0)));
    // u_sun_size is the half-size of LL's sun BILLBOARD; LL's sun texture only fills its
    // centre with the bright disc (soft corona outward), so the disc is ~30% of it and
    // the corona fades out to the billboard edge.
    float disc = u_sun_size * 0.3;
    float core = smoothstep(disc, disc * 0.8, ang);
    // Faint, fast-falling corona (LL's texture falloff): in HDR a linear 25% skirt
    // saturated into a disc 3x the real one once exposure lifted it.
    float skirt = pow(smoothstep(u_sun_size, disc, ang), 4.0) * 0.08;
    return min(pre + 2.0 * ssr.atmos[8].rgb * (core + skirt) * sun_gate, vec3(5.0));
}

// <GMDX> PHYSICAL NIGHT: the moon's disc (texture colour c, sRGB as LL samples it) over the sky
// pre value as an HDR light, gain = gmdxMoonDiscGain(); composited in linear light (an alpha
// blend of an HDR value in the pre space would harden the disc's soft edge). Never darker than
// LL's own disc (texture x moon_brightness), so a dusk moon with little light keeps its look.
vec3 gmdxMoonDiscOver(vec3 pre, vec3 c, float gain, float a)
{
    const vec3 ll = skyResolve(c * ssr.atmos[12].x);
    const vec3 lin = max(atmosSrgbToLinear(max(c, vec3(0.0))) * gain, ll);
    return physkyPre(mix(skyResolve(pre), lin, clamp(a, 0.0, 1.0)));
}

// <GMDX> PHYSICAL NIGHT: the moon's phase on its disc, from the real sun-moon geometry (EEP moves
// the two independently). n: the disc's sphere normal at the pixel (toward the viewer), sun and
// mdir unit directions in the same frame. The lit side faces the sun with a crisp terminator (the
// moon's dust scatters nearly flat to its edge, not like a Lambert ball), the dark side keeps a
// faint earthshine, strongest for a crescent (the Earth is then nearly full from the moon).
// The texture's detail is multiplied, never replaced. The disc's overall brightness by phase is
// the moon light's own (night-light's K&S factor).
const float kGmdxEarthshine = 0.04;
float gmdxMoonPhase(vec3 n, vec3 sun, vec3 mdir)
{
    const float lit = smoothstep(-0.03, 0.10, dot(n, sun));
    const float dark_share = 0.5 * (1.0 + dot(mdir, sun));   // 1 at new moon, 0 at full
    const float es = kGmdxEarthshine * dark_share;
    return es + (1.0 - es) * lit;
}

vec3 skyProceduralMoon(vec3 pre, vec3 d)
{
    vec3  u_sun_dir   = atmosToYUp(ssr.atmos[10].xyz);
    vec3  u_moon_dir  = atmosToYUp(ssr.atmos[11].xyz);
    float u_moon_size = ssr.atmos[11].w;
    float moon_gate = smoothstep(-0.1, 0.0, u_moon_dir.y);
    if (moon_gate <= 0.0 || u_moon_size <= 0.0) return pre;
    vec3 mdir = normalize(u_moon_dir);
    float cos_a = dot(d, mdir);
    if (cos_a <= 0.0) return pre;
    vec3 pole  = (abs(mdir.y) > 0.99) ? vec3(1.0, 0.0, 0.0) : vec3(0.0, 1.0, 0.0);
    vec3 right = normalize(cross(pole, mdir));
    vec3 up    = cross(mdir, right);
    vec3 perp  = d - mdir * cos_a;
    float u = dot(perp, right) / u_moon_size;
    float v = dot(perp, up)    / u_moon_size;
    float r2 = u * u + v * v;
    if (r2 >= 1.0) return pre;
    // sphere-shaded -> phases
    float w = sqrt(1.0 - r2);
    vec3 n = right * u + up * v - mdir * w;
    float lit = clamp(dot(n, normalize(u_sun_dir)), 0.0, 1.0);
    float edge = smoothstep(1.0, 0.8, r2);
    // <GMDX> physical night: the disc as a light (clean_atmos_common.glsl gmdxMoonDiscGain)
    const float hdr = gmdxMoonDiscGain();
    if (gmdxNightOn()) lit = gmdxMoonPhase(n, normalize(u_sun_dir), mdir);   // (the faint 0.05 floor became the earthshine)
    if (hdr > 0.0)
        return gmdxMoonDiscOver(pre, ssr.atmos[9].rgb * lit, hdr, edge * moon_gate);
    vec3 moon_col = ssr.atmos[9].rgb * ssr.atmos[12].x * (0.05 + 0.95 * lit);
    return mix(pre, clamp(moon_col * 2.0, vec3(0), vec3(5)), edge * moon_gate);
}

float skyHash13(vec3 p)
{
    p = fract(p * 0.1031);
    p += dot(p, p.zyx + 31.32);
    return fract((p.x + p.y) * p.z);
}

vec3 skyHash33(vec3 p)
{
    p = fract(p * vec3(0.1031, 0.1030, 0.0973));
    p += dot(p, p.yxz + 33.33);
    return fract((p.xxy + p.yxx) * p.zyx);
}

#include "gmdx_night_sky.glsl"   // <GMDX> the catalogue's stars, Milky Way, airglow, zodiacal light

// starsV/F for the star whose quad could cover world dir d (additive rgb * a). Must be
// called in uniform control flow (derivatives).
vec3 skyStars(vec3 d)
{
    float star_alpha = ssr.bodies[7].x;
    if (star_alpha < 0.001 || ssr.bodies[8].w < 0.0) return vec3(0.0);

    // Into the field's frame: LL rotates it by +angle about Z.
    float ca = cos(ssr.bodies[7].y), sa = sin(ssr.bodies[7].y);
    vec3 p = vec3(ca * d.x + sa * d.y, -sa * d.x + ca * d.y, d.z);

    // Cube-face grid cell.
    vec3 ap = abs(p);
    int axis = (ap.x >= ap.y && ap.x >= ap.z) ? 0 : ((ap.y >= ap.z) ? 1 : 2);
    float major = (axis == 0) ? p.x : ((axis == 1) ? p.y : p.z);
    vec2 fc = ((axis == 0) ? p.yz : ((axis == 1) ? p.xz : p.xy)) / max(abs(major), 1e-6);
    vec2 cell = min(floor((fc * 0.5 + 0.5) * SKY_STAR_GRID), vec2(SKY_STAR_GRID - 1.0));
    vec3 key = vec3(cell, float(axis * 2 + (major < 0.0 ? 1 : 0)));
    float h  = skyHash13(key);
    vec3  r  = skyHash33(key + 17.0);
    vec3  r2 = skyHash33(key + 53.0);

    // The star: inset in its cell so the whole quad (<= 36 units, both axes) stays inside.
    vec2 sfc = (cell + 0.2 + 0.6 * r.xy) / SKY_STAR_GRID * 2.0 - 1.0;
    float sg = major < 0.0 ? -1.0 : 1.0;
    vec3 s = (axis == 0) ? vec3(sg, sfc.x, sfc.y)
           : ((axis == 1) ? vec3(sfc.x, sg, sfc.y) : vec3(sfc.x, sfc.y, sg));
    s = normalize(s);

    // LL's quad (updateStarGeometry): anchored at the star, left = at x Z, up = at x left,
    // both scaled by sc (unnormalized, so quads shrink toward the zenith).
    float sc = (16.0 + 20.0 * r.z) / SKY_STAR_RADIUS;
    vec3 left = cross(s, vec3(0.0, 0.0, 1.0)) * sc;
    vec3 up   = cross(s, left);
    float ll  = max(dot(left, left), 1e-14);
    vec3 hit  = p / max(dot(p, s), 1e-4);
    vec3 off  = hit - s;
    float a = dot(off, left) / ll, b = dot(off, up) / ll;
    // corners: star (1,0), +up (1,1), +left+up (0,1), +left (0,0)
    vec2 uv = vec2(1.0 - a, b);
    vec2 dx = dFdx(uv), dy = dFdy(uv);

    // ~1000 stars (x RenderStarCount) over the 3 GRID^2 cells of the upper hemisphere.
    float prob = min(1.0, SKY_STAR_COUNT * ssr.sky_fx2.x / (3.0 * SKY_STAR_GRID * SKY_STAR_GRID));
    if (h >= prob || s.z <= 0.0 || a < 0.0 || a > 1.0 || b < 0.0 || b > 1.0) return vec3(0.0);

    vec4 col = skyTexClampGrad(ssr.bodies[8].w, uv, dx, dy);
    col.rgb *= vec3(0.75 + 0.25 * r2.x, 1.0, 0.75 + 0.25 * r2.y);
    // starsF: alpha = tex.a * smoothstep(0, .9, custom_alpha) * 32 * twinkle, where the
    // twinkle is fract() of the star-space vertex position.xy * mod(time, 1.25).
    vec2 screenpos = hit.xy * SKY_STAR_RADIUS * ssr.bodies[7].z;
    float twinkle = fract(screenpos.x + screenpos.y);
    float alpha = col.a * smoothstep(0.0, 0.9, star_alpha) * 32.0 * twinkle;
    return col.rgb * alpha;   // BT_ADD_WITH_ALPHA
}

// pre = LL's sky gbuffer value (skySceneryPre); returns it with the sun, moon and stars
// composited as LL's heavenly-body and star passes do.
vec3 skyBodiesComposite(vec3 pre, vec3 dir_world)
{
    vec3 d = normalize(dir_world);
    // Everything textured takes its derivatives here, in uniform control flow.
    vec2 sun_xy  = skyBillboard(d, 0);
    vec2 moon_xy = skyBillboard(d, 3);
    vec2 sun_uv  = 0.5 + 0.5 * sun_xy;
    vec2 moon_uv = 0.5 + 0.5 * moon_xy;
    vec2 sdx = dFdx(sun_uv),  sdy = dFdy(sun_uv);
    vec2 mdx = dFdx(moon_uv), mdy = dFdy(moon_uv);
    // <GMDX> GMDXPhysicalNight: the catalogue's stars in place of LL's random ones (uniform branch)
    const bool ns_on = ssr.bodies[7].w > 0.0;
    const bool ns_glow = ssr.bodies[4].w + ssr.bodies[5].w + ssr.bodies[6].w + nsSpace(16).w > 0.0;   // (+ the deep sky)
    const float ns_pix = max(max(length(dFdx(d)), length(dFdy(d))), 1e-6);
    const mat3 ns_sky = (ns_on || ns_glow) ? nsWorldToSky() : mat3(1.0);
    vec3 stars = ns_on ? nsStars(d, ns_sky * d, ns_pix) + nsPlanets(d, ns_sky * d, ns_pix) + nsSatellites(d, ns_pix)
                       : skyStars(d);   // <GMDX> + planets, satellites
    bool stars_hidden = false;
    vec3 d_yup = atmosToYUp(d);   // the procedural fallbacks work in LL's Y-up frame
    // <GMDX> ... and the Milky Way, airglow and zodiacal light, under the sun and moon
    if (ns_glow) pre = nsAddLinear(pre, nsSkyGlow(d, ns_sky));

    // ---- sun (sunDiscF): A/B mixed by the blend factor, alpha-blended ----
    if (ssr.bodies[8].x < 0.0)
    {
        pre = skyProceduralSun(pre, d_yup);
    }
    else if (ssr.bodies[0].w > 0.5 && skyInQuad(sun_xy))
    {
        vec4 c = skyTexClampGrad(ssr.bodies[8].x, sun_uv, sdx, sdy);
        if (ssr.bodies[1].w > 0.0 && ssr.bodies[8].y != ssr.bodies[8].x)
            c = mix(c, skyTexClampGrad(ssr.bodies[8].y, sun_uv, sdx, sdy), ssr.bodies[1].w);
        // <GMDX> space at altitude (clean_atmos_common.glsl gmdxSpace): no air to spread or redden it,
        // a sharper, whiter disc (the texture's soft glow tightened, its colour toward white).
        const float sp = gmdxSpace();
        if (sp > 0.0)
        {
            c.a = pow(clamp(c.a, 0.0, 1.0), 1.0 + 3.0 * sp);
            c.rgb = mix(c.rgb, vec3(max(max(c.r, c.g), c.b)), 0.7 * sp);
        }
        pre = mix(pre, c.rgb, clamp(c.a, 0.0, 1.0));
        stars_hidden = true;   // the sun quad writes depth (no discard)
    }

    // ---- moon (moonF): tex * moon_brightness, faded near the horizon ----
    // (<GMDX> physical night: an HDR light, gmdxMoonDiscOver; the EEP texture's detail and alpha kept)
    if (ssr.bodies[8].z < 0.0)
    {
        pre = skyProceduralMoon(pre, d_yup);
    }
    else if (ssr.bodies[3].w > 0.5 && skyInQuad(moon_xy))
    {
        vec4 c = skyTexClampGrad(ssr.bodies[8].z, moon_uv, mdx, mdy);
        if (c.a > 2.0 / 255.0)   // SL-14113: transparent texels neither draw nor hide stars
        {
            float mz = ssr.bodies[3].z;
            float fade = (mz > 0.0) ? clamp(mz * mz * 4.0, 0.0, 1.0) : 1.0;
            // <GMDX> physical night: the phase on the EEP texture (sphere normal from the billboard);
            // not on a creator's own moon texture (shadow_fit.w < 0), drawn as Second Life draws it
            if (gmdxNightOn() && ssr.shadow_fit.w >= 0.0)
            {
                const vec3 mc = normalize(ssr.bodies[3].xyz);
                const vec2 q = moon_xy / max(length(moon_xy), 1.0);
                const vec3 n = normalize(normalize(ssr.bodies[4].xyz) * q.x + normalize(ssr.bodies[5].xyz) * q.y
                                         - mc * sqrt(max(1.0 - dot(q, q), 0.0)));
                c.rgb *= gmdxMoonPhase(n, normalize(ssr.atmos[10].xyz), mc);
            }
            const float hdr = gmdxMoonDiscGain();   // <GMDX> physical night: the disc as a light
            if (hdr > 0.0) pre = gmdxMoonDiscOver(pre, c.rgb, hdr, c.a * fade);
            else           pre = mix(pre, c.rgb * ssr.atmos[12].x, clamp(c.a * fade, 0.0, 1.0));
            stars_hidden = true;
        }
    }

    // ---- stars (starsF, additive, behind the bodies) ----
    // <GMDX> the catalogue's in linear light
    if (!stars_hidden && ns_on) { if (any(greaterThan(stars, vec3(0.0)))) pre = nsAddLinear(pre, stars); }
    else if (!stars_hidden) pre += stars;
    return pre;
}
#endif   // CLEAN_SKY_SCENERY_ONLY
