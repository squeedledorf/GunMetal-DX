// vkclean — WindLight / EEP atmosphere: sky, lighting terms and haze.
//
// Ports of the GL clean pipeline's shaders, kept as close to verbatim as the
// forward/world-space setting allows:
//   skyHazePre()      <- skyV.glsl / cleanSkyFullF.glsl (per-pixel sky haze integral;
//                        LL's Y-up "lightnorm" frame inside)
//   atmosLightTerms() <- cleanDeferredLib cl_calcAtmosphericVarsLinear (sunlit/amblit)
//   atmosHaze()       <- cleanDeferredLib cl_calcAtmospherics (additive/atten)
//   atmosApplyHaze()  <- cleanHazeF.glsl composite
//
// All outputs are LINEAR HDR (the world renders into an RGBA16F target; the
// tonemap pass converts to display). World space is the agent frame, Z up.
//
// Requires the frame-info block instance `ssr` (clean_frame_info.glsl: a uniform buffer
// since #814) with its vec4 atmos[16],
// filled by LLCleanRendererVK::setAtmosphere from LLVKWorldFrame::atmos:
//   [0]  blue_horizon.rgb, haze_horizon
//   [1]  blue_density.rgb, haze_density
//   [2]  sun-or-moon light colour.rgb (haze sunlight), cloud_shadow
//   [3]  ambient.rgb (LL "ambient_color"), max_y
//   [4]  clamped light norm (world, z up), density_multiplier (<GMDX> physical night: the
//        WindLight sky's own light, gmdxSkyLight; the scene light's norm is atmosLightNorm())
//   [5]  glow.xyz, distance_multiplier
//   [6]  sun_moon_glow_factor, sky_hdr_scale, enabled, no emissive buffer (1 = vintage
//        mode, RenderEnableEmissiveBuffer off: atmosDropEmissive) (<GMDX> enabled = 2 - physical
//        night's ambient factor, 1 = stock: gmdxNightAmbient)
//   [7]  light direction (world, normalized; sun or moon), distance-fade fraction
//   [8]  sunlight_color.rgb, classic_mode
//   [9]  moonlight_color.rgb, sun_up (1/0)   (<GMDX> the EEP colour: with GMDXPhysicalNight the
//        moon's real strength is in [2], not here)
//   [10] TRUE sun dir (world), sun disc size
//   [11] TRUE moon dir (world), moon disc size
//   [12] moon_brightness, star_brightness, sky_sunlight_scale, sky_ambient_scale
//   [13..15] post/tonemap parameters (not read here)
// and its PER-FRAME HAZE CONSTANTS vec4 atmos_k[6] (#816; LLCleanRendererVK::fillAtmosK, from
// the same atmos[] floats, once per frame on the CPU instead of once per pixel here):
//   [0] sunlight after the light-path extinction, sunlight * exp(-light_atten / lightnorm.z)
//       (sRGB space: atmosScatter's sunlit_out), max_y
//   [1] the haze extinction per metre, max(blue_density + haze_density, 1e-6) *
//       density_multiplier * distance_multiplier (atten = exp(-[1].rgb * distance)), glow.x
//   [2] the in-scatter's glow-free part, blue_horizon * blue_weight * (cs + tmpAmbient) +
//       haze_horizon * haze_weight * tmpAmbient, glow.z
//   [3] its glow slope, haze_horizon * haze_weight * cs (additive = [2] + [3] * haze_glow),
//       sun_moon_glow_factor
//   [4] amblit before the per-pixel ambient factor, pow(tmpAmbient, 0.9) * 0.57,
//       the sun glow's near cut-off (m; #842, RenderVKHazeSunGlowNear)
//   [5] atmosLightTerms' final sunlit: (classic ? s : srgb_to_linear(s)) * sky_sunlight_scale
//       with s = [0].rgb, the sun glow's full-strength distance (m; #842)
//   (tmpAmbient = ambient + (1 - ambient) * cloud_shadow * 0.5, cs = [0].rgb * (1 - cloud_shadow),
//   blue / haze_weight = blue_density / haze_density over the clamped sum.)
// Only the ray (its length and direction: the glow) and the normal's scalar ambient factor
// vary per pixel. LOSSLESS UP TO ROUNDING: the constants are the same float formulas
// evaluated once (libm exp / pow on the CPU vs the GPU's ex2 / lg2 approximations: a few ulp)
// and two re-associations -- the extinction product (c * dm * distm) * len for
// (-c * (len * dm)) * distm, and [2] + [3] * glow for the two-term sum: max relative error
// ~2e-6 on additive / atten / sunlit / amblit, far below the RGBA16F target's 4.9e-4 step.

#define ATMOS_PI 3.14159265

vec3 atmosSrgbToLinear(vec3 cs)
{
    vec3 low  = cs / 12.92;
    vec3 high = pow((max(cs, vec3(0.0)) + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, lessThanEqual(cs, vec3(0.04045)));
}

vec3 atmosLinearToSrgb(vec3 c)
{
    c = clamp(c, vec3(0.0), vec3(1.0));
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0/2.4)) - 0.055, step(vec3(0.0031308), c));
}

vec3 atmosClampHDR(vec3 c)
{
    c = mix(c, vec3(1.0), isinf(c));
    c = mix(c, vec3(0.0), isnan(c));
    return clamp(c, vec3(0.0), vec3(11.2));
}

bool  atmosEnabled()  { return ssr.atmos[6].z > 0.5; }
// (#818) Spec constant 18 (clean_frame_info.glsl kSpecSky; LLCleanRendererVK SPEC_SKY): the lit
// shaders of a spec set compile ONE of lightSurface's classic / linear PBR + legacy paths; the
// renderer re-keys (generic pipelines meanwhile) the moment the environment's sky flips.
bool  atmosClassic()  { return kSpecSky >= 0 ? kSpecSky == 1 : ssr.atmos[8].w > 0.5; }
float atmosHdrScale() { return ssr.atmos[6].y; }
// Vintage mode (RenderEnableEmissiveBuffer off): LL's G-buffer has no emissive target, so
// deferred (opaque / masked) PBR and PBR-terrain emissive is dropped; alpha-blended PBR
// (forward) and HUDs keep theirs. `flags` = the instance flags (2u blend, 8u HUD).
bool  atmosDropEmissive(uint flags) { return ssr.atmos[6].w > 0.5 && (flags & (2u | 8u)) == 0u; }
vec3  atmosLightDir() { return ssr.atmos[7].xyz; }
// <GMDX> PHYSICAL NIGHT (GMDXPhysicalNight; newview/gmdxnightlight.h): ssr.gmdx_misc.w is 0 when
// off (the old Second Life night), non-zero when on. The scene light (atmos[2] / [7]) already
// carries the moon's real strength then; the sky below follows it.
bool  gmdxNightOn()   { return ssr.gmdx_misc.w != 0.0; }
// The moon's light as a fraction of the sun's, for the physical sky's second light (0: none).
float gmdxMoonSky()   { return max(ssr.gmdx_misc.w, 0.0); }
// The WindLight sky's own light (gmdxnightlight.h): the dome, its table, the reflection probes'
// sky and the classic 2D clouds. Its direction is atmos[4]; vol_clouds[3].w its strength q, >= 0
// the sun's colour x q, < 0 the moon's (the sun's colour x -q in linear light). Night on only.
// moonGain scales the moon's part in linear light (the classic 2D clouds: gmdxCloudMoonGain).
vec3 gmdxSkyLight(float moonGain)
{
    const float q = ssr.vol_clouds[3].w;
    if (q >= 0.0) return ssr.atmos[8].rgb * q;
    const vec3 c = max(atmosSrgbToLinear(ssr.atmos[8].rgb) * -q * moonGain, vec3(0.0));
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), c));
}
vec3 gmdxSkyLight() { return gmdxSkyLight(1.0); }
// THE CLOUDS' MOONLIGHT (GMDXNightCloudLight, vol_clouds[2].w; gmdxnightlight.h): the moon's light
// where it lights the clouds is x this -- 1 by day, the setting once the moon gives the scene's
// light (by its share at dusk). The disc, the sky and the ground keep GMDXMoonlightScale.
float gmdxCloudMoonGain() { return gmdxNightOn() ? max(ssr.vol_clouds[2].w, 0.0) : 1.0; }
// THE NIGHT'S AMBIENT (GMDXNightAmbient; gmdxnightlight.h): EEP's ambient light at night follows the
// moon, x this (linear; 1 by day, with physical night off and with the atmosphere off). Classic
// lighting and the WindLight sky work in LL's sRGB-ish space: x its 1 / 2.4 power there.
float gmdxNightAmbient() { return clamp(2.0 - ssr.atmos[6].z, 0.0, 1.0); }
float gmdxNightAmbientSrgb() { return pow(gmdxNightAmbient(), 1.0 / 2.4); }
// Its haze glow factor (getSunMoonGlowFactor's: 1 for the sun; the moon's is below 1, no glow).
float gmdxSkyGlowFactor() { return ssr.vol_clouds[3].w > 0.0 ? 1.0 : 0.0; }
// The SCENE light's clamped norm (fillAtmosK's extinction, the haze glow on geometry): atmos[4],
// or with physical night (which gives atmos[4] to the sky's light) atmos[7]'s, z >= -0.1.
vec3 atmosLightNorm()
{
    if (!gmdxNightOn()) return ssr.atmos[4].xyz;
    return vec3(ssr.atmos[7].xy, max(ssr.atmos[7].z, -0.1));
}

// World (Z up) -> WindLight (Y up) frame, LLEnvironment::toLightNorm's permutation.
vec3 atmosToYUp(vec3 w) { return vec3(w.y, w.z, w.x); }

// ---------------------------------------------------------------------------
// <GMDX> PHYSICAL SKY (RenderVKPhysicalSky; clean_physky.glsl, gmdxphysskyvk.cpp). Hillaire's
// sky-view and transmittance LUTs (draw set 0 bindings 57 / 58, linear HDR radiance) stand in
// for skyHazePre's WindLight dome, and colour the haze; the haze AMOUNT (atten) and all
// surface lighting stay WindLight's, so regions keep their look apart from the sky's colour.
// Both outputs go back through LL's sky "pre" space (physkyPre: the inverse of skyResolve) so
// clouds, the sun / moon billboards, stars and every consumer composite exactly as before.
// ---------------------------------------------------------------------------
#define PHYSKY_P(i) ssr.physky[i]
#include "clean_physky.glsl"
layout(set = 0, binding = 57) uniform sampler2D uPkSkyView;   // <GMDX> (46 / 47 are the volumetric clouds, 54 / 55 the VS instances)
layout(set = 0, binding = 58) uniform sampler2D uPkTrans;

bool physkyOn() { return ssr.physky[4].w > 0.5; }
// <GMDX> SPACE AT ALTITUDE (GMDXSpaceAltitude; newview/gmdxspacealt.h): 0 .. 1 as the camera climbs
// from GMDXSpaceStart to GMDXSpaceFull (frame info cluster_rt.w, unused upstream). 0: no change.
float gmdxSpace() { return ssr.cluster_rt.w; }
// The camera's own height (km) for what lies around it (cloud lighting): the physical sky's viewer
// height is lifted by the space amount (physSkyFill), the clouds below keep their real air.
float gmdxCamHeightKm() { return gmdxSpace() > 0.0 ? clamp(ssr.cam_origin.z * 0.001, 0.0, 60.0) : max(pkViewHeight(), 0.0); }
// The planet's limb at the horizon: a viewer lifted into near space sees the bright band of the air
// below the horizontal (the planet's dip); the elevations are stretched so it sits on the horizon
// (the horizon sea's edge) instead of under it. Zenith and nadir stay.
vec3 gmdxSpaceLimb(vec3 d, float h)
{
    const float s = gmdxSpace();
    if (s <= 0.0) return d;
    const float dip = acos(clamp(pkBottom() / h, -1.0, 1.0)) * s;
    const float e = asin(clamp(d.z, -1.0, 1.0));
    const float e2 = -dip + e * (1.0 + sign(e) * dip / 1.5707963);
    const float l = length(d.xy);
    const vec2 hz = l > 1e-6 ? d.xy / l : vec2(1.0, 0.0);
    return vec3(hz * cos(e2), sin(e2));
}
// <GMDX> Physical night (gmdxphysskyvk.cpp): the light above is the sun and the moon is a second
// one, in the sky-view table's lower half, while physky[4].w = 2; its direction is atmos[11].
bool physkyMoonOn()  { return ssr.physky[4].w > 1.5; }
vec3 physkyMoonDir() { return normalize(ssr.atmos[11].xyz); }

// One half of the sky-view table (0 the light, 1 the moon) along d, for a light from Ldir. The
// table is two 108-row halves: v stays on its own half's texel centres (clamp to edge per half).
vec3 physkyViewHalf(vec3 d, vec3 fwd, vec3 side, bool ground, float h, vec3 Ldir, float half_)
{
    vec2 lop = vec2(dot(Ldir, fwd), dot(Ldir, side));
    float ll = length(lop);
    float lightViewCos = ll > 1e-4 ? lop.x / ll : 1.0;
    vec2 uv = pkSkyViewUv(ground, d.z, lightViewCos, h);
    uv.y = (clamp(uv.y * PK_SV_H, 0.5, PK_SV_H - 0.5) + half_ * PK_SV_H) * (0.5 / PK_SV_H);
    return textureLod(uPkSkyView, uv, 0.0).rgb;
}

// <GMDX> THE MOON'S DISC AS A LIGHT (GMDXMoonDisc, frame info shadow_fit.w; 0 = LL's disc): the linear
// radiance of a white texel of the EEP moon texture, a plain multiple (kGmdxMoonDisc x the setting) of
// the moon's illuminance in the sky's own units (the physical sky's moon light, or the WindLight sky's
// at night), so it keeps one ratio to the clouds the moon lights through every phase, moon_brightness
// and exposure: far over white on screen, where the bloom takes it. 0 by day, with physical night
// off and for a creator's own moon texture (shadow_fit.w < 0) (the caller then draws LL's disc as before).
const float kGmdxMoonDisc = 50.0;
float gmdxMoonDiscGain()
{
    const float k = ssr.shadow_fit.w;
    if (k <= 0.0 || !gmdxNightOn()) return 0.0;
    const vec3 luma = vec3(0.2126, 0.7152, 0.0722);
    const float E = physkyOn() ? (physkyMoonOn() ? dot(max(pkLight(), vec3(0.0)), luma) * gmdxMoonSky() : 0.0)
                               : dot(atmosSrgbToLinear(max(ssr.atmos[8].rgb, vec3(0.0))), luma)
                                 * max(-ssr.vol_clouds[3].w, 0.0) * atmosHdrScale();
    return k * kGmdxMoonDisc * E;
}

// <GMDX> TWILIGHT GLOW (physical night): a floor under the sky-view table's twilight while the sun
// is below the horizon. The table keeps the sun's twilight (its light stays the sun below the
// horizon), but the light reaches the upper air along a grazing path: in a dense or red-heavy EEP
// medium (Midnight: density x1.7, a grey blue_density = 3x the red Rayleigh) that path's
// transmittance all but vanishes, and with the sky's own light EEP's (dim on a night sky) the glow
// went black by -3 degrees. The floor, toward the sun's azimuth and hugging the horizon (a faint
// band all round above it), in units of the sun light's luminance: kGmdxTwilight at the horizon
// toward the sun with the sun just down, kGmdxTwilightFall stops less per degree below that (-3:
// clear, -8: faint, gone by -12). Strength: about a day sky's zenith (~0.03-0.05 of the light) in
// the glow's red at -3, so it reads through the auto exposure's 2 stops (0.015 was ~1/10 of that,
// physically about right, and under the table everywhere on screen). Its colour is a warm glow tinted by the EEP sun colour's hue
// (a night sky's bluish sun makes it rose / violet). Game look, plain constants to tune by eye.
// Only raises the sky where the table gives less, so a sky whose twilight shows keeps its own.
const float kGmdxTwilight = 0.12;
const float kGmdxTwilightFall = 0.7;
vec3 gmdxTwilightFloor(vec3 d)
{
    const vec3 s = pkLightDir();
    const float e = degrees(asin(clamp(s.z, -1.0, 1.0)));
    if (!gmdxNightOn() || e > 0.5 || e < -12.0 || d.z < -0.05) return vec3(0.0);
    const float amp = kGmdxTwilight * exp2(kGmdxTwilightFall * min(e, 0.0))
                    * smoothstep(0.5, -1.5, e) * smoothstep(-12.0, -9.0, e) * smoothstep(-0.05, 0.0, d.z);
    const float sl = length(s.xy), dl = length(d.xy);
    const float az = (sl > 1e-4 && dl > 1e-4) ? 0.5 + 0.5 * dot(s.xy / sl, d.xy / dl) : 0.5;
    const float h = max(d.z, 0.0);
    const float toward = az * az * exp(-h / (0.10 + 0.012 * -e));   // ~6 deg high, widening as the sun sinks
    const float band = 0.15 * exp(-h / 0.35);
    const vec3 L = max(pkLight(), vec3(0.0));
    const float lum = dot(L, vec3(0.2126, 0.7152, 0.0722));
    const vec3 hue = L / max(lum, 1e-6);
    return amp * lum * (toward * mix(vec3(1.6, 0.85, 0.45), hue, 0.3)
                      + band * mix(vec3(0.74, 0.98, 1.96), hue, 0.3));
}

// Sky radiance along a world direction from the viewer: the sky-view LUT (single + multiple
// scattering of the sun / moon) plus the ambient colour scattered toward the viewer.
vec3 physkySkyRaw(vec3 dir_world)
{
    float h = pkBottom() + max(pkViewHeight(), 0.001);
    vec3 d = gmdxSpaceLimb(normalize(dir_world), h);   // <GMDX> space at altitude
    vec3 pos = vec3(0.0, 0.0, h);
    vec3 side = cross(vec3(0.0, 0.0, 1.0), d);
    float sl = length(side);
    side = sl > 1e-4 ? side / sl : vec3(0.0, 1.0, 0.0);
    vec3 fwd = cross(side, vec3(0.0, 0.0, 1.0));
    bool ground = pkRaySphere(pos, d, pkBottom()) >= 0.0;
    vec3 L = physkyViewHalf(d, fwd, side, ground, h, pkLightDir(), 0.0);
    L = max(L, gmdxTwilightFloor(d));   // <GMDX>
    // (the moon's half is lit at the sun's illuminance: fp16-safe)
    if (physkyMoonOn()) L += gmdxMoonSky() * physkyViewHalf(d, fwd, side, ground, h, physkyMoonDir(), 1.0);
    vec3 T = textureLod(uPkTrans, pkTransUv(h, max(d.z, 0.0)), 0.0).rgb;
    return L + pkAmbient() * pkRayleighTint() * (vec3(1.0) - T);
}

// Highlight roll-off (RenderVKPhysicalSkyHighlight; 0 = none): a low sun's forward glow is far
// brighter than any WindLight sky and burned the horizon white; compress the luminance, keep
// the hue.
vec3 physkyRolloff(vec3 L)
{
    float W = PHYSKY_P(6).z;
    return W > 0.0 ? L / (1.0 + dot(L, vec3(0.2126, 0.7152, 0.0722)) / W) : L;
}

vec3 physkySky(vec3 dir_world) { return physkyRolloff(physkySkyRaw(dir_world)); }

// Mean extinction (1/km) of the air at the viewer's height: the aerial perspective's.
float physkyExtinction()
{
    PkMedium m = pkMedium(vec3(0.0, 0.0, pkBottom() + max(pkViewHeight(), 0.0)));
    return dot(m.extinction, vec3(1.0 / 3.0));
}

// Linear HDR -> LL's sky pre space: skyResolve(physkyPre(c)) == c (no clamp).
vec3 physkyPre(vec3 c)
{
    vec3 x = max(c / max(atmosHdrScale(), 1e-4), vec3(0.0));
    return mix(x * 12.92, 1.055 * pow(x, vec3(1.0 / 2.4)) - 0.055, step(vec3(0.0031308), x));
}
// </GMDX>

// ---------------------------------------------------------------------------
// Haze along a ray from the camera (cl_calcAtmospherics). rel_pos = frag - camera
// (world). additive is sRGB-space (LL convention); atten the transmittance.
// sunlit_out is the attenuated sun/moon light (sRGB space, pre scale).
// ---------------------------------------------------------------------------
// The per-pixel part (#816): only the ray's length (the extinction) and direction (the sun
// glow) vary; every per-frame term comes from atmos_k (see the header).
void atmosScatterHaze(vec3 rel_pos, out vec3 additive, out vec3 atten)
{
    const float max_y = ssr.atmos_k[0].w;
    // altitude clamp so short rays don't overshoot the sky dome (LL: eye y; here world up)
    if (abs(rel_pos.z) > max_y) rel_pos *= (max_y / abs(rel_pos.z));

    float rel_pos_len  = max(length(rel_pos), 1e-4);
    vec3  rel_pos_norm = rel_pos / rel_pos_len;

    atten = exp(-ssr.atmos_k[1].rgb * rel_pos_len);

    // LL (atmosphericsFuncs calcAtmosphericVars, from softenLight / haze / alpha / water) dots
    // the EYE-space ray with lightnorm, a WORLD vector in WindLight's Y-up frame
    // (getClampedLightNorm, never moved to eye space); light_dir is eye space, so that second
    // dot is frame-free. Both factors reach 1 at the sun only for a level camera facing west,
    // so LL's surfaces almost never get the sun hotspot. Dotting world with world here put
    // it on every surface in the sun's direction: a sun disc on ceilings and walls (skybox
    // interiors) under skies with a tight, strong glow. The main view's rotation stands in
    // for LL's per-view eye space (probe / mirror captures differ slightly).
    float haze_glow = dot(mat3(ssr.views[CLEAN_VIEW].cur_view) * rel_pos_norm, atmosToYUp(atmosLightNorm()));   // lightnorm
    haze_glow *= max(0.0, dot(ssr.atmos[7].xyz, rel_pos_norm));   // light_dir
    haze_glow = 1.0 - haze_glow;
    haze_glow = max(haze_glow, 0.001);
    haze_glow *= ssr.atmos_k[1].w;   // glow.x
    haze_glow = clamp(pow(haze_glow, ssr.atmos_k[2].w), -100000.0, 100000.0);   // glow.z
    // NEAR-GEOMETRY SUN GLOW (#842): LL's glow is the in-scatter of SUNLIT haze, but nothing
    // here knows whether the air in front of a surface is lit. Toward the sun it never is
    // for a near surface: the surface itself (V ~ L) shadows the air between it and the eye,
    // and so does any roof over the camera -- yet the spike above (up to 1e5 for a tight
    // glow focus) times even a few metres of heavy haze saturated the min(10) below into a
    // hard-edged sun disc on the inside of a roof. Nearer than atmos_k[4].w the glow is
    // capped at its 90-degree value (pow(glow.x, glow.z): the whole anti-solar hemisphere's,
    // since max(0, light_dir.V) zeroes the angle term there), so only the EXCESS toward the
    // sun -- the halo / disc -- is cut, ramping back in by atmos_k[5].w
    // (RenderVKHazeSunGlowNear, x6). Off-axis haze is unchanged at every distance; far
    // geometry (and the sky pass, which does not come through here) keeps LL's halo.
    // Both 0: LL behaviour.
    if (ssr.atmos_k[5].w > 0.0)
    {
        const float g90 = clamp(pow(ssr.atmos_k[1].w, ssr.atmos_k[2].w), -100000.0, 100000.0);
        haze_glow = mix(min(haze_glow, g90), haze_glow,
                        smoothstep(ssr.atmos_k[4].w, ssr.atmos_k[5].w, rel_pos_len));
    }
    haze_glow += 0.25;
    haze_glow *= ssr.atmos_k[3].w;   // sun_moon_glow_factor

    additive = ssr.atmos_k[2].rgb + ssr.atmos_k[3].rgb * haze_glow;
    additive *= vec3(1.0) - atten;
    additive  = min(additive, vec3(10.0));

    // <GMDX> Physical sky: the same amount of haze, coloured by the sky behind it (looking down,
    // the horizon's). Weighted in the pre space as WindLight's own in-scatter is (atmosApplyHaze
    // adds srgb_to_linear(additive * 2) * hdr), so near haze stays as faint as Classic's.
    // Aerial perspective (RenderVKPhysicalSkyAerial): the physical air's own extinction over the
    // ray, stretched by the distance scale (SL's draw distances are short for real air), on top of
    // WindLight's; grey (consumers read atten.r).
    // Coloured by the horizon in the ray's compass direction, whatever its elevation: a few
    // hundred metres of air scatter about the horizon's colour; the sky higher up is the deep
    // blue of the whole atmosphere above and tinted everything above eye level blue.
    if (physkyOn())
    {
        vec2 flat_dir = rel_pos_norm.xy;
        float fl = length(flat_dir);
        vec3 hd = vec3(fl > 1e-3 ? flat_dir / fl : vec2(1.0, 0.0), 0.03);
        float t = dot(atten, vec3(1.0 / 3.0)) * exp(-physkyExtinction() * rel_pos_len * 0.001 * pkAerialScale());
        atten = vec3(t);
        additive = 0.5 * physkyPre(physkySky(hd)) * (1.0 - t);
    }
}

// cl_calcAtmospherics. sunlit_out / amblit_out are per-frame constants (atmos_k [0] / [4]).
void atmosScatter(vec3 rel_pos, out vec3 sunlit_out, out vec3 amblit_out,
                  out vec3 additive, out vec3 atten)
{
    atmosScatterHaze(rel_pos, additive, atten);
    sunlit_out = ssr.atmos_k[0].rgb;
    amblit_out = ssr.atmos_k[4].rgb;
}

// cl_ambientLighting: soft back-light so shadowed areas keep detail.
float atmosAmbientLighting(vec3 norm, vec3 light_dir)
{
    float a = min(abs(dot(norm, light_dir)), 1.0);
    a *= 0.5;
    a *= a;
    return 1.0 - a;
}

// cl_calcAtmosphericVarsLinear: sunlit/amblit for lighting at a surface. In classic
// mode they stay in LL's sRGB-ish space (the classic recombination expects it).
// (#816: sunlit is a per-frame constant, atmos_k[5]; amblit keeps its per-pixel ambient
// factor INSIDE the sRGB -> linear conversion, so only its base is precomputed.)
// atmosLightTerms' amblit alone (no haze): callers that need the sky ambient of a surface
// without its haze terms (#872: the split lighting's sharp-reflection pass).
vec3 atmosAmbientTerm(vec3 norm)
{
#ifdef CLEAN_LPROBE_NO_ATMOS
    return vec3(0.2);   // (register probe)
#endif
    vec3 amblit = ssr.atmos_k[4].rgb * atmosAmbientLighting(norm, atmosLightDir());
    if (!atmosClassic())
    {
        amblit = atmosSrgbToLinear(amblit);
        amblit = vec3(dot(amblit, vec3(0.2126, 0.7152, 0.0722)));
    }
    return amblit * ssr.atmos[12].w   // sky_ambient_scale
         * (atmosClassic() ? gmdxNightAmbientSrgb() : gmdxNightAmbient());   // <GMDX> the night's ambient
}

void atmosLightTerms(vec3 rel_pos, vec3 norm, out vec3 sunlit, out vec3 amblit,
                     out vec3 additive, out vec3 atten)
{
#ifdef CLEAN_LPROBE_NO_ATMOS
    sunlit = vec3(1.0); amblit = vec3(0.2); additive = vec3(0.0); atten = vec3(1.0);   // (register probe)
    return;
#endif
    atmosScatterHaze(rel_pos, additive, atten);
    sunlit = ssr.atmos_k[5].rgb;   // (classic or linear) x sky_sunlight_scale
    amblit = atmosAmbientTerm(norm);
}

// cleanHazeF composite on LINEAR colour: lin * atten.r + srgb_to_linear(additive*2) * hdr.
vec3 atmosApplyHaze(vec3 lin, vec3 additive, vec3 atten)
{
    return lin * atten.r + atmosSrgbToLinear(additive * 2.0) * atmosHdrScale();
}

vec3 atmosApply(vec3 lin, vec3 world_pos, vec3 cam_pos)
{
    if (!atmosEnabled()) return lin;
    vec3 additive, atten;
    atmosScatterHaze(world_pos - cam_pos, additive, atten);
    return atmosApplyHaze(lin, additive, atten);
}
// atmosApplyRay = atmosApply with the ray's additive / atten already in hand (#816: the caller ran
// atmosLightTerms / atmosScatterHaze on the SAME ray world_pos - cam_pos -- bit-identical).
vec3 atmosApplyRay(vec3 lin, vec3 additive, vec3 atten)
{
    return atmosEnabled() ? atmosApplyHaze(lin, additive, atten) : lin;
}

// ---------------------------------------------------------------------------
// SKY (cleanSkyFullF.glsl / skyV+skyF). dir_world = view ray (world, z up).
// skyHazePre() is skyV's haze colour (before skyF doubles it); optic_d is skyF's
// rainbow/halo coordinate. The textured parts of LL's sky -- rainbow + 22-degree halo,
// sun/moon billboards, stars -- live in clean_sky_bodies.glsl (they need the bindless
// table). skyColorPre() is the haze alone in LL's sky gbuffer space; LL writes
// clamp(color*2, 0, 5) and its soften SKIP_ATMOS branch resolves it as
// srgb_to_linear(c) * sky_hdr_scale (skyResolve).
// ---------------------------------------------------------------------------
const float ATMOS_DOME_RADIUS = 15000.0;

// (#819) The haze colour depends on the view direction only through n = normalize(the dome
// point -- d * ATMOS_DOME_RADIUS + 50 m up, Y up): its height n.y and its cosine to the light
// norm, n . lightnorm. skyHazeCore takes those two scalars (the sky-view LUT's coordinates,
// clean_sky_lut.comp); skyHazePre derives them from the direction. The dome point's scaled
// length LL computes (rel_pos *= max_y / rel_pos.y above the horizon, -32000 / rel_pos.y below)
// is max_y / n.y resp. 32000 / -n.y.
vec3 skyHazeCore(float ny, float nl)
{
    vec3  blue_horizon = ssr.atmos[0].rgb;  float haze_horizon = ssr.atmos[0].w;
    vec3  blue_density = ssr.atmos[1].rgb;  float haze_density = ssr.atmos[1].w;
    float cloud_shadow = ssr.atmos[2].w;
    vec3  ambient_color= ssr.atmos[3].rgb;  float max_y        = ssr.atmos[3].w;
    vec3  lightnorm    = atmosToYUp(ssr.atmos[4].xyz);
    float density_multiplier = ssr.atmos[4].w;
    vec3  glow         = ssr.atmos[5].xyz;
    float sun_moon_glow_factor = ssr.atmos[6].x;
    bool  sun_up       = ssr.atmos[9].w > 0.5;

    // ---- atmosphere (per-pixel port of the dome VS math) ----
    // (rel_pos = d * ATMOS_DOME_RADIUS + (0, 50, 0), rescaled to max_y above / -32000 below the
    // horizon: the same direction, n; its length from n.y alone -- see skyHazePre)
    float rel_pos_len  = ny > 0.0 ? max_y / ny : (ny < 0.0 ? -32000.0 / ny : ATMOS_DOME_RADIUS);
    float rel_pos_lightnorm_dot = nl;
    vec3  rel_pos_norm = vec3(0.0, ny, 0.0);   // (only .y is read below)

    vec3 sunlight = sun_up ? ssr.atmos[8].rgb : ssr.atmos[9].rgb * 0.7;
    // <GMDX> Physical night: the dome (and the reflection probes, which see this sky) has its own
    // light, the sun's colour while it lights the dome, then the moon at its real strength
    // (gmdxSkyLight; lightnorm above is its direction).
    if (gmdxNightOn())
    {
        sunlight = gmdxSkyLight();
        sun_moon_glow_factor = gmdxSkyGlowFactor();
        sun_up = true;   // (the glow's moon dimming below: no glow in the moon's part anyway)
    }

    vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);

    vec3 combined_haze = max(abs(blue_density) + vec3(abs(haze_density)), vec3(1e-6));
    vec3 blue_weight   = blue_density / combined_haze;
    vec3 haze_weight   = haze_density / combined_haze;

    float off_axis = 1.0 / max(1e-6, max(0., rel_pos_norm.y) + lightnorm.y);
    sunlight *= exp(-light_atten * off_axis);

    float density_dist = rel_pos_len * density_multiplier;
    combined_haze = exp(-combined_haze * density_dist);

    float haze_glow = 1.0 - rel_pos_lightnorm_dot;
    haze_glow = max(haze_glow, .001);
    haze_glow *= glow.x;
    haze_glow = pow(haze_glow, glow.z);
    haze_glow = (sun_moon_glow_factor < 1.0) ? 0.0 : (sun_moon_glow_factor * (haze_glow + 0.25));
    // #848: the moon's halo is this glow, so moon_brightness dims it too (not only the disc).
    // Unchanged from the default 0.5 up; below it the glow falls off linearly to nothing.
    if (!sun_up) haze_glow *= clamp(ssr.atmos[12].x * 2.0, 0.0, 1.0);

    // <GMDX> the night's ambient (gmdxNightAmbient; 1 by day): the dome's fill light follows the moon
    const float night_amb = gmdxNightAmbientSrgb();
    vec3 color = (blue_horizon * blue_weight * (sunlight + ambient_color * night_amb)
               + (haze_horizon * haze_weight) * (sunlight * haze_glow + ambient_color * night_amb));
    color *= (1. - combined_haze);

    vec3 ambient = (ambient_color + max(vec3(0), (1. - ambient_color)) * cloud_shadow * 0.5) * night_amb;
    vec3 sunlight_under_cloud = sunlight * max(0.0, (1. - cloud_shadow));
    vec3 add_below_cloud = (blue_horizon * blue_weight * (sunlight_under_cloud + ambient)
                         + (haze_horizon * haze_weight) * (sunlight_under_cloud * haze_glow + ambient));

    combined_haze = sqrt(combined_haze);
    color += (add_below_cloud - color) * (1. - sqrt(combined_haze));

    return color;
}

// The dome direction n of a view ray (Y up, unit) -- skyHazeCore's coordinates.
vec3 skyDomeDir(vec3 dir_world)
{
    const vec3 d = atmosToYUp(normalize(dir_world));
    return normalize(d * ATMOS_DOME_RADIUS + vec3(0.0, 50.0, 0.0));
}

// <GMDX> SPACE AT ALTITUDE, the classic (WindLight) sky: what the dome keeps of its haze colour at dome
// height ny (pre space; 1 = all), darker toward the zenith, a band kept along the horizon (the air
// seen edge on). Not in the reflection-probe captures: surface lighting stays WindLight's.
const float kGmdxSpaceZenith = 0.06;    // the zenith's share at the full amount (pre space: near black)
const float kGmdxSpaceLimb   = 0.06;    // the horizon band's height (dome n.y, ~3.5 degrees)
float gmdxSpaceClassicKeep(float ny)
{
#ifdef PHYSKY_NOT_IN_PROBES
    return 1.0;
#else
    const float s = gmdxSpace();
    if (s <= 0.0) return 1.0;
    return mix(1.0, mix(kGmdxSpaceZenith, 1.0, exp(-max(ny, 0.0) / kGmdxSpaceLimb)), s);
#endif
}

vec3 skyHazePre(vec3 dir_world, out float optic_d)
{
    const vec3 n = skyDomeDir(dir_world);
    optic_d = dot(n, atmosToYUp(ssr.atmos[4].xyz));   // skyF's rainbow/halo coordinate
    // <GMDX> Physical sky: Hillaire's radiance, in the pre space skyColorPre doubles. Not in the
    // reflection-probe captures (PHYSKY_NOT_IN_PROBES): the probes light every PBR surface, and
    // the physical sky's deeper blue cooled the whole world; lighting stays WindLight's. (Here,
    // not in skyHazeCore: the #819 core sees only the dome direction's two scalars.)
#ifndef PHYSKY_NOT_IN_PROBES
    if (physkyOn()) return 0.5 * physkyPre(physkySky(dir_world));
#endif
    return skyHazeCore(n.y, optic_d) * gmdxSpaceClassicKeep(n.y);   // <GMDX> space at altitude
}

// LL's sky gbuffer value without the textured parts (clamp(color*2, 0, 5), pre-
// linearization): reflection fallbacks. Clouds alpha-blend in this space.
vec3 skyColorPre(vec3 dir_world)
{
    float optic_d;
    return clamp(skyHazePre(dir_world, optic_d) * 2.0, vec3(0), vec3(5));
}

// Resolve LL's sky gbuffer value as its soften SKIP_ATMOS branch does: linear HDR.
vec3 skyResolve(vec3 pre) { return atmosSrgbToLinear(pre) * atmosHdrScale(); }

// SKY-VIEW LOOKUP TABLE (#819, RenderVKSkyLUT; LLCleanRendererVK recordSkyLut, clean_sky_lut.comp):
// skyColor (the resolved, linear HDR haze sky: ~6 exp, a pow, 2 sqrt and the sRGB decode per
// call) is a function of the TWO scalars skyHazeCore takes, so a per-frame 2D table of it is
// exact up to interpolation -- no azimuth axis, no sun-centred reparameterization needed:
//   u = sqrt(theta / pi), theta = acos(n . lightnorm): texels crowd toward the light, where the
//       haze glow (pow(1 - cos, glow.z), a spike for a tight glow focus) varies fastest;
//   v = 0.5 + 0.5 sign(n.y) sqrt(|n.y|): texels crowd toward the horizon (the dome's
//       above / below switch at n.y = 0 falls exactly on the texel boundary at v = 0.5).
// Bilinear. Consumers: the probe-miss sky (forward / deferred lighting), the water reflection's
// sky, the far-clip fade target, the RT miss shading -- every skyColor() of a shader that
// defines CLEAN_SKY_LUT (draw set 0 binding 52). The sky pass itself keeps the analytic haze
// (it also needs optic_d and is what you look at directly). ssr.ao_up.z = 1: the table holds
// this frame's sky (else, and without CLEAN_SKY_LUT, the analytic path).
vec2 skyLutUV(vec3 n, vec3 lightnorm)
{
    const float th = acos(clamp(dot(n, lightnorm), -1.0, 1.0));
    return vec2(sqrt(th * (1.0 / 3.14159265)), 0.5 + 0.5 * sign(n.y) * sqrt(abs(n.y)));
}
#ifdef CLEAN_SKY_LUT
layout(set = 0, binding = 52) uniform sampler2D uSkyViewLut;   // RGBA16F, linear / clamp, GENERAL
#endif
vec3 skyColor(vec3 dir_world)
{
#ifdef CLEAN_SKY_LUT
#ifndef PHYSKY_NOT_IN_PROBES
    if (ssr.ao_up.z > 0.5 && !physkyOn())   // <GMDX> the #819 table holds the WindLight haze
#else
    if (ssr.ao_up.z > 0.5)
#endif
    {
        const vec3 n = skyDomeDir(dir_world);   // <GMDX> space at altitude (as skyHazePre; ~linear)
        return textureLod(uSkyViewLut, skyLutUV(n, atmosToYUp(ssr.atmos[4].xyz)), 0.0).rgb
             * pow(gmdxSpaceClassicKeep(n.y), 2.2);
    }
#endif
    return skyResolve(skyColorPre(dir_world));
}

// ---------------------------------------------------------------------------
// FAR-CLIP DISTANCE FADE. LL's haze integral is physically faint at typical draw
// distances (default Midday: ~4% at 512 m), so geometry used to end in a hard line
// at the far clip. Over the last `frac` of the view depth, blend (ease-in) toward
// the sky in this exact view direction, so the draw distance dissolves into it.
// view_depth is along the camera forward -- matches the planar far clip exactly.
// ---------------------------------------------------------------------------
float atmosFadeFactor(float view_depth, float far_clip, float frac)
{
    float x = clamp((view_depth - far_clip * (1.0 - frac)) / max(far_clip * frac, 1e-3), 0.0, 1.0);
    return pow(x, 1.6);
}

// What a fading fragment dissolves into. At or above the horizon the sky is what lies
// behind it, so fade to the sky (HDR, as drawn). Below the horizon there is no sky
// behind: skyColor() there is saturated horizon haze x2 x HDR scale, and fading the
// ground toward it (looking down from altitude, where all of it is near the far clip)
// blew it out to glowing white. There, fade to the aerial-perspective haze colour
// instead: the colour atmosScatter's in-scatter converges to with distance, in the same
// linear range as lit surfaces (no sky HDR boost). Blend by view elevation, no seam.
vec3 atmosFadeHaze(vec3 sky, float below, vec3 additive, vec3 atten)
{
    // additive = inscatter * (1 - atten): divide the transmittance back out.
    // <GMDX> In the same range atmosApplyHaze puts the haze in (x HDR scale): without it, under an
    // HDR sky the faded ground (neighbour regions seen from altitude) went far darker than the
    // hazed ground before it -- dark squares at the draw distance.
    vec3 inscatter = additive / max(vec3(1.0) - atten, vec3(1e-3));
    vec3 haze = atmosSrgbToLinear(min(inscatter * 2.0, vec3(1.0))) * atmosHdrScale();
    return mix(sky, haze, below);
}
// <GMDX> HORIZON WATER: past the draw distance the world is open water out to the horizon. The sky
// pass draws it below the horizon (camera above the water), and the ground and water at the draw
// distance fade into it -- region tiles no longer end in a jagged line against the sky.
bool camUnderwater();
float waterHeight();
float horizonWaterAmount(vec3 dir) { return smoothstep(0.0, -0.01, dir.z); }   // a crisp sea horizon
vec3 horizonWater(vec3 dir_world, float cam_above_water);

vec3 atmosFadeTarget(vec3 rel, float cam_z)
{
    vec3 sky = skyColor(rel);
    vec3 dir = rel / max(length(rel), 1e-4);
    if (!camUnderwater() && cam_z > waterHeight())
    {
        float w = horizonWaterAmount(dir);
        return w > 0.0 ? mix(sky, horizonWater(dir, cam_z - waterHeight()), w) : sky;
    }
    float below = smoothstep(-0.02, -0.25, dir.z);
    if (below <= 0.0) return sky;
    vec3 additive, atten;
    atmosScatterHaze(rel, additive, atten);
    return atmosFadeHaze(sky, below, additive, atten);
}
// ... with the ray's haze already in hand (#816: no second atmosScatter on the same ray).
vec3 atmosFadeTarget(vec3 rel, vec3 additive, vec3 atten, float cam_z)
{
    vec3 sky = skyColor(rel);
    vec3 dir = rel / max(length(rel), 1e-4);
    if (!camUnderwater() && cam_z > waterHeight())
    {
        float w = horizonWaterAmount(dir);
        return w > 0.0 ? mix(sky, horizonWater(dir, cam_z - waterHeight()), w) : sky;
    }
    float below = smoothstep(-0.02, -0.25, dir.z);
    if (below <= 0.0) return sky;
    return atmosFadeHaze(sky, below, additive, atten);
}

vec3 atmosDistanceFade(vec3 lin, vec3 world_pos, vec3 cam_pos, vec3 cam_fwd, float far_clip)
{
#ifdef CLEAN_LPROBE_NO_ATMOS
    return lin;   // (register probe)
#endif
    float frac = ssr.atmos[7].w;
    if (frac <= 0.0 || far_clip <= 0.0 || !atmosEnabled()) return lin;
    vec3  rel = world_pos - cam_pos;
    float f = atmosFadeFactor(dot(rel, cam_fwd), far_clip, frac);
    if (f <= 0.0) return lin;
    return mix(lin, atmosFadeTarget(rel, cam_pos.z), f);
}
// HAZE REUSE (#816): additive / atten = atmosScatterHaze(world_pos - cam_pos), computed by the
// caller for its haze (atmosLightTerms / atmosApply on the same ray): the fade target's
// below-horizon branch reuses them instead of scattering the ray again. Bit-identical.
vec3 atmosDistanceFade(vec3 lin, vec3 world_pos, vec3 cam_pos, vec3 cam_fwd, float far_clip,
                       vec3 additive, vec3 atten)
{
    float frac = ssr.atmos[7].w;
    if (frac <= 0.0 || far_clip <= 0.0 || !atmosEnabled()) return lin;
    vec3  rel = world_pos - cam_pos;
    float f = atmosFadeFactor(dot(rel, cam_fwd), far_clip, frac);
    if (f <= 0.0) return lin;
    return mix(lin, atmosFadeTarget(rel, additive, atten, cam_pos.z), f);
}
// The fade's slope on the colour (#827 surfaceLocalGain): mix(lin, target, f) keeps 1 - f of lin;
// 1 wherever the haze-reuse atmosDistanceFade above (surfacePost's) returns lin unchanged. Same
// conditions: keep in step.
float atmosDistanceFadeKeep(vec3 world_pos, vec3 cam_pos, vec3 cam_fwd, float far_clip)
{
    float frac = ssr.atmos[7].w;
    if (frac <= 0.0 || far_clip <= 0.0 || !atmosEnabled()) return 1.0;
    float f = atmosFadeFactor(dot(world_pos - cam_pos, cam_fwd), far_clip, frac);
    if (f <= 0.0) return 1.0;
    return 1.0 - f;
}

// ---------------------------------------------------------------------------
// WATER FOG (LL waterFogF getWaterFogViewNoClip), world space, plane z = water height.
// rel = point - eye. Eye ABOVE water: fog starts where the view ray enters the water;
// eye BELOW: from the eye, with the surface-light falloff pow(F, ks * eye_depth).
// Returns (inscattered fog colour, transmittance), LINEAR.
// ---------------------------------------------------------------------------
bool  camUnderwater() { return ssr.water[5].w > 0.5; }
float waterHeight()   { return ssr.water[3].w; }

// The open sea seen along dir (below the horizon) from cam_above_water metres up: the sky mirrored
// at a grazing angle (Fresnel, F0 0.02) over the water's own colour, hazed over the distance to
// it -- far out it meets the horizon sky. Linear HDR, as skyColor.
vec3 horizonWater(vec3 dir_world, float cam_above_water)
{
    vec3 d = normalize(dir_world);
    float c = max(-d.z, 1e-4);
    vec3 mirrored = normalize(vec3(d.xy, c));
    float F = 0.02 + 0.98 * pow(1.0 - c, 5.0);
    vec3 col = mix(atmosSrgbToLinear(ssr.water[4].rgb), skyColor(mirrored), F);
    vec3 additive, atten;
    atmosScatterHaze(d * (max(cam_above_water, 0.5) / c), additive, atten);
    return atmosApplyHaze(col, additive, atten);
}

vec4 waterFogLL(vec3 rel, float eye_above)
{
    const float F = 0.98;
    float kd = ssr.water[4].w;
    float ks = ssr.water[5].x;
    float len = max(length(rel), 1e-4);
    vec3  view = rel / len;
    float es = -view.z;                                    // -dot(view, plane normal)
    vec3  hit = (eye_above > 0.0 && es > 1e-4) ? view * (eye_above / es) : vec3(0.0);
    float l  = max(length(rel - hit), 0.1);
    float e0 = max(-eye_above, 0.0);
    float t1 = -kd * pow(F, ks * e0);
    float t2 = kd + ks * es;
    float t3 = pow(F, t2 * l) - 1.0;
    float L  = pow(clamp(t1 / t2 * t3, 0.0, 1.0), 1.0 / 1.7);
    float transmit = pow(F, l * kd);
    return vec4(atmosSrgbToLinear(ssr.water[4].rgb) * L, max(pow(transmit, 1.7), 0.0));
}

vec3 applyWaterFog(vec3 lin, vec3 rel, float eye_above)
{
    vec4 f = waterFogLL(rel, eye_above);
    return lin * f.a + f.rgb;
}

// LL applySkyAndWaterFog. An eye ABOVE the water only sees underwater geometry through
// the water surface, whose shader fogs its refraction itself -- so the world pass only
// water-fogs when the eye is UNDER the water and the fragment is below the plane.
bool waterFogged(vec3 world_pos) { return camUnderwater() && world_pos.z < waterHeight(); }

vec3 atmosOrWaterFog(vec3 lin, vec3 world_pos, vec3 cam, vec3 additive, vec3 atten)
{
    if (waterFogged(world_pos)) return applyWaterFog(lin, world_pos - cam, cam.z - waterHeight());
    return atmosEnabled() ? atmosApplyHaze(lin, additive, atten) : lin;
}

