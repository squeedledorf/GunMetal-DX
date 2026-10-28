// <GMDX> NIGHT SKY (GMDXPhysicalNight; gmdxnightsky.cpp fills the parameters and makes the
// data). The Yale Bright Star Catalogue's 9096 stars at their real places, brightness and
// colours, in place of LL's 1000 random ones; and what else glows in a dark sky: the Milky Way
// (ESO/S. Brunier's panorama), airglow and the zodiacal light. All added to the sky in linear
// light, under the sun / moon billboards and the clouds (the stars as LL's are: hidden behind
// the bodies; the glow under their alpha blend). Included by clean_sky_bodies.glsl (needs ssr,
// uTextures, skyHash13, atmosSrgbToLinear).
//
// Frame info (all zero: the old sky, exactly):
//   bodies[7].w  catalogue star scale: > 0 = these stars (EEP star_brightness x GMDXStarBrightness
//                x what twilight / moonlight leave of the contrast), 0 = LL's stars
//   bodies[2].w  latitude (rad): the celestial pole's elevation over the north (+Y) horizon
//   bodies[9].z  bindless index of the star grid (gmdxnightskygrid.h), < 0 none (yet)
//   bodies[5].w  airglow at the zenith (linear; the unit the diffuse glow is measured in; also
//                the dark sky's navy, kNsNightBlue)
//   bodies[4].w  zodiacal light (linear, per zenith-airglow unit of nsZodiacal)
//   bodies[6].w  Milky Way (its texture's 1.0, the core's brightest)
//   bodies[9].w  bindless index of the Milky Way (gmdx_milkyway.jpg, Galactic equirectangular)
//   (the diffuse glow's three carry what twilight / moonlight leave of the contrast too)
//   sky_fx2.y    the sun's ecliptic longitude (rad): the season, which stars a night shows
//   sky_fx2.z    world time (s, wraps hourly): scintillation
//   sky_fx2.w    scintillation amount (GMDXStarScintillation, 1 = default)
//
// Units: linear, the space skyResolve multiplies by the HDR sky scale (atmosSrgbToLinear(pre)).
//
// SPACE BLOCK: the top 24 vec4s of ssr.meteors (the Lightbox meteors keep 56 x 3 below it:
// llcleanrenderervk.cpp kVKMeteorHardMax; nothing else reads them), gmdxnightsky.cpp fillFrame.
// Zeros (physical night off) while bodies[7].w is 0, so read only under ns_on / ns_glow.
//   [0..4]   planets: J2000 direction xyz, light (nsStars' units, px^2 x linear; 0 = none)
//   [5..9]   their colours (luminance 1)
//   [10..15] satellites: WORLD direction xyz, light (sunlit share applied; 0 = none)
//   [16]     x faintest star magnitude drawn, y star size, z colour saturation, w deep sky
//            (zenith-airglow units x GMDXDeepSky, washed out like the Milky Way)
//   [17]     the Milky Way: x turn along its band (rad), y tilt (rad), z exponent on its texture
//            (1; < 1 spreads its faint outskirts: the fantasy sky)

const int   NS_N = 64;                    // gmdxnightskygrid.h: N, K, W x H, SPLAT
const int   NS_K = 8;
const ivec2 NS_SIZE = ivec2(1024, 512);
const float NS_SPLAT = 0.00872665;        // 0.5 deg
const float NS_TAU = 6.2831853;
const int NS_SPACE = 192 - 24;
vec4 nsSpace(int i) { return ssr.meteors[NS_SPACE + i]; }


// World (x east, y north, z up) -> J2000 equatorial (x the March equinox, z the north celestial
// pole): the pole over the north horizon at the latitude, the sky turned about it so the sun's
// hour angle is the catalogue sun's on the season's date (gmdxnightsky.cpp).
mat3 nsWorldToSky()
{
    const float lat = ssr.bodies[2].w, lam = ssr.sky_fx2.y, obliquity = 0.409093;   // 23.44 deg
    const vec3 pole = vec3(0.0, cos(lat), sin(lat));
    const vec3 sun = ssr.bodies[0].xyz;
    vec3 a2 = sun - pole * dot(sun, pole);
    a2 = dot(a2, a2) > 1e-8 ? normalize(a2) : vec3(1.0, 0.0, 0.0);   // (the sun on the pole: any)
    const vec3 b2 = normalize(vec3(cos(lam), sin(lam) * cos(obliquity), 0.0));
    // e = z (d.pole) + b2 (d.a2) + (z x b2) (d.(pole x a2))
    return mat3(vec3(0.0, 0.0, 1.0), b2, vec3(-b2.y, b2.x, 0.0)) * transpose(mat3(pole, a2, cross(pole, a2)));
}

// A clear night's extinction by the air: Kasten & Young 1989 air mass, ~0.12 / 0.20 / 0.32 mag
// per air mass in R / G / B (V ~0.2): 10^(-0.4 k X).
vec3 nsExtinction(float sin_h)
{
    const float h = degrees(asin(clamp(sin_h, 0.0, 1.0)));
    const float air_mass = 1.0 / (max(sin_h, 0.0) + 0.50572 * pow(h + 6.07995, -1.6364));
    return exp2(-1.3287712 * vec3(0.12, 0.20, 0.32) * air_mass);
}

// Linear light added onto LL's sky pre space (its skyResolve stays linear in it).
vec3 nsAddLinear(vec3 pre, vec3 lin)
{
    const vec3 x = atmosSrgbToLinear(pre) + max(lin, vec3(0.0));
    return mix(x * 12.92, 1.055 * pow(x, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), x));
}

// The catalogue stars around world direction d (e: the same in the sky frame; pix: radians per
// pixel). Each is a Gaussian of 0.65 px holding its light (V magnitude: x 2.512 a magnitude),
// widened with the same light where its peak would pass 2, so bright stars grow instead of
// only clipping; never wider than the grid's splat radius (a 3-sigma cut there).
vec3 nsStars(vec3 d, vec3 e, float pix)
{
    const int ti = int(ssr.bodies[9].z);
    if (ti < 0 || ssr.bodies[7].w < 1e-9) return vec3(0.0);   // (1e-12: the catalogue's, none to draw)
    const uint tu = uint(ti);
    if (any(notEqual(textureSize(uTextures[nonuniformEXT(tu)], 0), NS_SIZE))) return vec3(0.0);
    const float sin_h = d.z;
    const float above = smoothstep(-0.01, 0.01, sin_h);
    if (above <= 0.0) return vec3(0.0);

    // The cell (gmdxnightskygrid.h cell, same arithmetic).
    const vec3 ae = abs(e);
    const int axis = (ae.x >= ae.y && ae.x >= ae.z) ? 0 : ((ae.y >= ae.z) ? 1 : 2);
    const float major = e[axis];
    const vec2 fc = vec2(axis == 0 ? e.y : e.x, axis == 2 ? e.y : e.z) / abs(major);
    const ivec2 c = ivec2(clamp(floor((fc * 0.5 + 0.5) * float(NS_N)), vec2(0.0), vec2(float(NS_N - 1))));
    const ivec2 row = ivec2(c.x * NS_K * 2, (axis * 2 + (major < 0.0 ? 1 : 0)) * NS_N + c.y);

    const float scale = ssr.bodies[7].w;
    const vec4 opt = nsSpace(16);
    const float sig_max = NS_SPLAT / (3.0 * pix);
    // Scintillation: two tones a star, stronger toward the horizon, where its colours part too.
    const float twinkle = ssr.sky_fx2.w * (0.05 + 0.35 * (1.0 - smoothstep(0.0, 0.5, sin_h)));
    const vec3 chroma = vec3(0.0, 0.11, 0.22) * (1.0 - sin_h);
    vec3 sum = vec3(0.0);
    for (int k = 0; k < NS_K; ++k)
    {
        const vec4 b = texelFetch(uTextures[nonuniformEXT(tu)], row + ivec2(2 * k + 1, 0), 0);
        if (b.a <= 0.0) break;   // the cell's list ends
        const uvec4 a = uvec4(round(texelFetch(uTextures[nonuniformEXT(tu)], row + ivec2(2 * k, 0), 0) * 255.0));
        const vec2 o = vec2(float(a.x * 256u + a.y), float(a.z * 256u + a.w)) / 65535.0 * 2.0 - 1.0;
        vec3 s = vec3(o, 1.0 - abs(o.x) - abs(o.y));
        if (s.z < 0.0) s.xy = (1.0 - abs(o.yx)) * vec2(o.x >= 0.0 ? 1.0 : -1.0, o.y >= 0.0 ? 1.0 : -1.0);
        s = normalize(s);

        const float mag = (round(b.a * 255.0) - 1.0) / 25.0 - 2.0;
        // GMDXStarLimitMagnitude: fainter ones fade out over half a magnitude (the list runs
        // brightest first, so the rest of it is fainter still); GMDXStarSize: wider by it, the
        // peak kept.
        const float fade = 1.0 - smoothstep(opt.x - 0.5, opt.x, mag);
        if (fade <= 0.0) break;
        const float light = scale * 13.3 * exp2(-1.3287712 * mag) * fade * opt.y * opt.y;   // px^2 x linear; V 6 peaks ~0.02
        const float sig = min(max(0.65 * opt.y, sqrt(light / (NS_TAU * 2.0))), sig_max);
        const float x = length(e - s) / (pix * sig);
        if (x >= 3.0) continue;
        const float g = (exp(-0.5 * x * x) - 0.011109) / 0.988891;
        const vec3 col = max(mix(vec3(1.0), b.rgb / max(dot(b.rgb, vec3(0.2126, 0.7152, 0.0722)), 1e-3), opt.z),
                             vec3(0.0));   // luminance 1, GMDXStarSaturation
        const float h1 = skyHash13(s * 4096.0), h2 = fract(h1 * 113.7);
        const vec3 tw = 1.0 + twinkle * (0.6 * sin(NS_TAU * (fract((2.0 + 3.0 * h1) * ssr.sky_fx2.z + h1) + chroma))
                                       + 0.4 * sin(NS_TAU * (fract((7.0 + 5.0 * h2) * ssr.sky_fx2.z + h2) + 2.0 * chroma)));
        sum += col * (light / (NS_TAU * sig * sig) * g) * max(tw, vec3(0.0));   // (GMDXStarScintillation > 1 can dip it below 0)
    }
    return sum * nsExtinction(sin_h) * above;
}

// A steady point of `light` (nsStars' units) r pixels away, core sigma sig px.
float nsPoint(float light, float r, float sig)
{
    return light * exp(-0.5 * r * r / (sig * sig)) / (NS_TAU * sig * sig);
}

// The planets around world direction d (e: in the sky frame; pix: radians per pixel): steady
// (planets do not twinkle: discs, not points), a core sized like a bright star's and a faint
// glow round it (kNsPlanetGlow of the light, sigma kNsPlanetGlowPx), so Venus and Jupiter read as
// a small disc.
const float kNsPlanetGlow = 0.04;
const float kNsPlanetGlowPx = 5.0;
vec3 nsPlanets(vec3 d, vec3 e, float pix)
{
    const float sin_h = d.z;
    const float above = smoothstep(-0.01, 0.01, sin_h);
    if (above <= 0.0) return vec3(0.0);
    const vec4 opt = nsSpace(16);
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 5; ++i)
    {
        const vec4 p = nsSpace(i);
        if (p.w <= 0.0) continue;
        const float r = length(e - p.xyz) / pix;
        if (r > 4.0 * kNsPlanetGlowPx) continue;
        const float sig = clamp(sqrt(p.w / (NS_TAU * 2.0)), 0.8, 2.5) * opt.y;
        const float k = (1.0 - kNsPlanetGlow) * nsPoint(p.w, r, sig) + kNsPlanetGlow * nsPoint(p.w, r, kNsPlanetGlowPx);
        sum += max(mix(vec3(1.0), nsSpace(5 + i).rgb, opt.z), vec3(0.0)) * k;
    }
    return sum * nsExtinction(sin_h) * above;
}

// The satellites around world direction d: small steady points, sunlight's colour.
vec3 nsSatellites(vec3 d, float pix)
{
    const float sin_h = d.z;
    const float above = smoothstep(-0.01, 0.01, sin_h);
    if (above <= 0.0) return vec3(0.0);
    const float sig = 0.7 * nsSpace(16).y;
    float sum = 0.0;
    for (int i = 10; i < 16; ++i)
    {
        const vec4 s = nsSpace(i);
        if (s.w <= 0.0) continue;
        const float r = length(d - s.xyz) / pix;
        if (r < 4.0 * sig) sum += nsPoint(s.w, r, sig);
    }
    return sum * vec3(1.0, 0.97, 0.92) * nsExtinction(sin_h) * above;
}

// J2000 equatorial -> Galactic (the Hipparcos catalogue's rotation, ESA 1997).
vec3 nsGalactic(vec3 e)
{
    return vec3(dot(vec3(-0.0548755604, -0.8734370902, -0.4838350155), e),
                dot(vec3( 0.4941094279, -0.4448296300,  0.7469822445), e),
                dot(vec3(-0.8676661490, -0.1980763734,  0.4559837762), e));
}

// The Milky Way at sky-frame direction e: ESO/S. Brunier's panorama (CC BY 4.0), its stars taken
// out (the catalogue draws those). Equirectangular in Galactic coordinates: l = 0 in the middle,
// growing leftward; b = +90 at the top (LL's rows run bottom-up: v = 1 at the top). Level 0: the
// texels are larger than pixels at any usual field of view, and there is no seam at l = 180.
vec3 nsMilkyWay(vec3 e)
{
    const int ti = int(ssr.bodies[9].w);
    if (ti < 0) return vec3(0.0);
    vec3 g = nsGalactic(e);
    const vec4 mw = nsSpace(17);
    if (mw.y != 0.0)   // GMDXMilkyWayTilt: about the line to the galactic centre
    {
        const float c = cos(mw.y), s = sin(mw.y);
        g = vec3(g.x, c * g.y - s * g.z, s * g.y + c * g.z);
    }
    if (mw.x != 0.0)   // GMDXMilkyWayRotation: along the band
    {
        const float c = cos(mw.x), s = sin(mw.x);
        g = vec3(c * g.x - s * g.y, s * g.x + c * g.y, g.z);
    }
    const vec2 uv = vec2(0.5 - atan(g.y, g.x) / NS_TAU, 0.5 + asin(clamp(g.z, -1.0, 1.0)) / 3.14159265);
    const vec3 c = pow(atmosSrgbToLinear(textureLod(uTextures[nonuniformEXT(uint(ti))], uv, 0.0).rgb), vec3(mw.z > 0.0 ? mw.z : 1.0));
    return max(mix(vec3(dot(c, vec3(0.2126, 0.7152, 0.0722))), c, nsSpace(16).z), vec3(0.0));   // GMDXStarSaturation
}

// THE DARK SKY'S COLOUR (game look, not physics): a moonless night reads as a photograph shows it,
// dark navy, never the olive the real airglow's green and the zodiacal light's warm white gave
// once the night exposure lifted them over the whole sky. kNsNightBlue is that navy (linear, per
// zenith-airglow unit; luma ~0.9), a little lighter toward the horizon. Not dimmed by the air's
// extinction (it stands in for light scattered in the air itself).
const vec3 kNsNightBlue = vec3(0.60, 0.90, 2.00);
// Airglow: the upper air's own faint light (the 557.7 nm oxygen green leads), from a shell ~90 km
// up; toward the horizon the line of sight runs through more of it (van Rhijn: 1 / sqrt(1 - (R /
// (R + h))^2 cos^2 elevation), ~6x at the horizon). Only that excess over the zenith is drawn, a
// faint green-grey tint low over the horizon (kNsAirglowTint) on the navy; extinction then dims
// it at the very horizon: strongest ~10-20 deg up. 0 at the zenith.
const float kNsAirglowTint = 0.25;
vec3 nsAirglow(float sin_h)
{
    const float k = 0.97236;   // (6371 / 6461)^2
    return kNsAirglowTint * vec3(0.75, 1.0, 0.70) * (inversesqrt(max(1.0 - k * (1.0 - sin_h * sin_h), 1e-3)) - 1.0);
}

// Zodiacal light: sunlight off the interplanetary dust along the ecliptic, brightest toward the
// sun (the cone after dusk and before dawn). After Leinert et al. 1998, in S10 units: ~60 at the
// ecliptic poles, along the ecliptic ~330 at 90 deg from the sun and ~2000 at 30; per zenith
// airglow (~150 S10). The ecliptic is drawn through the sun where EEP puts it.
const float kNsZodiacal = 0.35;
float nsZodiacal(vec3 d, vec3 e, vec3 e_sun)
{
    const float elong = degrees(acos(clamp(dot(d, ssr.bodies[0].xyz), -1.0, 1.0)));
    const vec3 pole = vec3(0.0, -0.397777, 0.917482);   // the ecliptic's north pole (J2000)
    const vec3 n = normalize(pole - e_sun * dot(pole, e_sun));
    const float beta = degrees(asin(clamp(dot(e, n), -1.0, 1.0)));
    const float along = 180.0 + 150.0 * pow(90.0 / max(elong, 20.0), 2.3);
    // Game look: only the cone toward the sun, at kNsZodiacal of its strength; the faint glow it
    // keeps over the rest of the sky (the ~60 S10 floor and the band opposite the sun) tinted
    // the whole night sky warm grey.
    const float cone = 1.0 - smoothstep(60.0, 110.0, elong);
    return kNsZodiacal * cone * (along - 60.0) * exp(-abs(beta) / (6.0 + 0.25 * elong)) / 150.0;
}

// DEEP SKY (GMDXDeepSky): soft blobs at the real places (J2000), game look: sizes and colours as
// long-exposure photographs show them, not as the eye does. RA, Dec (deg), sigma along / across
// the long axis (deg).
const vec4 kNsDeep[5] = vec4[5](
    vec4( 10.68,  41.27, 1.00, 0.30),    // M31, the Andromeda galaxy
    vec4( 83.82,  -5.39, 0.30, 0.25),    // M42, the Orion nebula
    vec4( 80.89, -69.76, 3.00, 2.40),    // the Large Magellanic Cloud
    vec4( 13.19, -72.83, 1.40, 0.80),    // the Small Magellanic Cloud
    vec4( 56.75,  24.12, 0.60, 0.50));   // M45: the Pleiades' blue haze (its stars are the catalogue's)
// The long axis' position angle (deg, north through east) and a core's share (pow(blob, 25): a core
// a fifth the size).
const vec2 kNsDeepShape[5] = vec2[5](vec2(35.0, 2.0), vec2(0.0, 1.0), vec2(170.0, 0.3), vec2(45.0, 0.3), vec2(0.0, 0.0));
// Colour x peak in zenith-airglow units (the Milky Way's core is ~3).
const vec3 kNsDeepCol[5] = vec3[5](vec3(0.35, 0.32, 0.27), vec3(0.50, 0.28, 0.38), vec3(1.00, 1.05, 1.15),
                                   vec3(0.60, 0.63, 0.70), vec3(0.15, 0.19, 0.26));
vec3 nsDeepSky(vec3 e, float sat)
{
    vec3 sum = vec3(0.0);
    for (int i = 0; i < 5; ++i)
    {
        const vec4 o = kNsDeep[i];
        const float ra = radians(o.x), dec = radians(o.y);
        const vec3 c = vec3(cos(dec) * cos(ra), cos(dec) * sin(ra), sin(dec));
        const float cd = dot(e, c);
        if (cd < cos(radians(4.0 * o.z))) continue;
        const vec3 east = vec3(-sin(ra), cos(ra), 0.0), north = cross(c, east);
        const float pa = radians(kNsDeepShape[i].x);
        const vec3 off = e / cd - c;   // (gnomonic: radians near the centre)
        const float u = degrees(dot(off, north * cos(pa) + east * sin(pa))) / o.z;
        const float v = degrees(dot(off, east * cos(pa) - north * sin(pa))) / o.w;
        const float g = exp(-0.5 * (u * u + v * v));
        const vec3 col = kNsDeepCol[i];
        sum += max(mix(vec3(dot(col, vec3(0.2126, 0.7152, 0.0722))), col, sat), vec3(0.0))
             * (g + kNsDeepShape[i].y * pow(g, 25.0));
    }
    return sum;
}

// The night sky's diffuse light toward world direction d (linear), sky frame m (nsWorldToSky).
vec3 nsSkyGlow(vec3 d, mat3 m)
{
    const float sin_h = d.z;
    const float above = smoothstep(-0.01, 0.01, sin_h);
    if (above <= 0.0) return vec3(0.0);
    const vec3 e = m * d;
    vec3 c = ssr.bodies[5].w * nsAirglow(sin_h);
    if (ssr.bodies[4].w > 0.0) c += ssr.bodies[4].w * nsZodiacal(d, e, m * ssr.bodies[0].xyz)
                                 * vec3(1.0, 0.92, 0.88);   // sunlight's colour
    if (ssr.bodies[6].w > 0.0) c += ssr.bodies[6].w * nsMilkyWay(e);
    if (nsSpace(16).w > 0.0) c += nsSpace(16).w * nsDeepSky(e, nsSpace(16).z);
    const vec3 navy = ssr.bodies[5].w * kNsNightBlue * (1.0 + 0.5 * (1.0 - sin_h) * (1.0 - sin_h));
    return (navy + c * nsExtinction(sin_h)) * above;
}
