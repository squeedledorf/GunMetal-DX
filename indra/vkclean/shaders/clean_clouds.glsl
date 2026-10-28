// vkclean — WindLight / EEP cloud layer: per-pixel port of LL's cloud dome
// (class1/deferred cloudsV.glsl + cloudsF.glsl, drawn by LLDrawPoolWLSky::
// renderSkyCloudsDeferred over the sky).
//
// LL draws the clouds on the WL dome mesh: a spherical cap (radius DOME_RADIUS, polar
// angle <= PI/8, LLVOWLSky::calcPhi) whose centre sits DOME_OFFSET * DOME_RADIUS below
// the camera, so the cap is a shallow curved layer overhead that meets the horizon.
// Texcoords are planar in the cap's unit-sphere x/z (LLVOWLSky::buildStripsBuffer).
// Here the view ray is intersected with that sphere per pixel and the vertex-shader
// math runs on the hit point, then the fragment math as-is.
//
// Output composites in LL's sky gbuffer space (skyColorPre: clamp(color*2, 0, 5)),
// where LL alpha-blends the cloud colour, before the SKIP_ATMOS resolve (skyResolve).
//
// Requires: clean_frame_info.glsl (ssr.clouds / ssr.atmos), clean_atmos_common.glsl,
// and the bindless table `uTextures` (set 1, binding 0) with GL_EXT_nonuniform_qualifier.
// Layout of ssr.clouds (LLVKWorldFrame::clouds, filled in llappviewer.cpp):
//   [0] cloud_color.rgb, cloud_scale
//   [1] cloud_pos_density1.xyz (scroll applied, x negated as LL), cloud_variance
//   [2] cloud_pos_density2.xyz, noise blend factor
//   [3] noise texture idx A, idx B (bindless), 0, enabled

#include "gmdx_cloud_corona.glsl"   // <GMDX>

const float CLOUD_DOME_RADIUS = 15000.0;          // LLSettingsSky::DOME_RADIUS
const float CLOUD_CAM_HEIGHT  = 0.96 * 15000.0;   // DOME_OFFSET * DOME_RADIUS
const float CLOUD_CAP_COS     = 0.92387953;       // cos(PI/8): LL's dome cap edge

vec4 cloudNoise(vec2 uv)
{
    int ia = int(ssr.clouds[3].x), ib = int(ssr.clouds[3].y);
    vec4 a = texture(uTextures[nonuniformEXT(uint(ia))], uv);
    if (ib == ia || ssr.clouds[2].w <= 0.0) return a;
    vec4 b = texture(uTextures[nonuniformEXT(uint(ib))], uv);
    return mix(a, b, ssr.clouds[2].w);
}

// <GMDX> The physical cloud layer's direct light from one light (E from Ld) toward dw. moon: the
// moon's silver-lining lobe mixed in (gmdx_cloud_common.glsl gcPhaseMoon: g 0.8, 35%; straight on
// ~2.2x, 90 degrees off ~10% less); the sun's light keeps the one lobe.
float cloudHG4pi(float g, float c) { return (1.0 - g * g) / pow(max(1.0 + g * g - 2.0 * g * c, 1e-4), 1.5); }
vec3 cloudPhysLit(vec3 E, vec3 Ld, vec3 dw, float cloud_r, vec3 cloud_color, bool moon)
{
    // (A sun just below the horizon still lights the layer, until the planet's shadow reaches it.)
    if (pkRaySphere(vec3(0.0, 0.0, cloud_r), Ld, pkBottom()) >= 0.0) return vec3(0.0);
    vec3  at_cloud = E * textureLod(uPkTrans, pkTransUv(cloud_r, Ld.z), 0.0).rgb * step(-0.1, Ld.z);
    float hg = cloudHG4pi(0.6, dot(dw, Ld));   // x 4 pi
    if (moon) hg = mix(hg, cloudHG4pi(0.8, dot(dw, Ld)), 0.35);   // (gcPhaseMoon's kGcMoonG / kGcMoonW)
    return at_cloud * cloud_color * (0.35 + 0.65 * hg) / PK_PI;
}

// <GMDX> The physical cloud layer's corona from one light (gmdx_cloud_corona.glsl): single
// scattering into the droplets' diffraction peak through a layer of optical depth
// tau = -ln(1 - alpha) along the view, tau exp(-tau), over alpha (the composite multiplies by it).
// Iridescence: the layer's thin parts (alpha below 0.6) hold smaller droplets.
vec3 cloudPhysCorona(vec3 E, vec3 Ld, vec3 dw, float cloud_r, vec3 cloud_color, float alpha)
{
    const vec3 ph = gcoPhase(dw, Ld, gcoDropletScale(alpha / 0.6));
    if (ph == vec3(0.0) || pkRaySphere(vec3(0.0, 0.0, cloud_r), Ld, pkBottom()) >= 0.0) return vec3(0.0);
    const vec3 at_cloud = E * textureLod(uPkTrans, pkTransUv(cloud_r, Ld.z), 0.0).rgb * step(-0.1, Ld.z);
    const float a = clamp(alpha, 1e-4, 0.999);
    return at_cloud * gcoAlbedo(cloud_color) * ph * (-log(1.0 - a) * (1.0 - a) / a);
}

// pre = LL's sky gbuffer value in this direction; returns it with the clouds blended over.
vec3 cloudComposite(vec3 pre, vec3 dir_world)
{
    if (ssr.clouds[3].w < 0.5 || ssr.clouds[3].x < 0.0 || !atmosEnabled()) return pre;
    // Volumetric layer live (#719, RenderVKVolumetricClouds 1): it replaces this dome in every
    // view (main: clean_vol_cloud_composite.frag; probes / mirror: clean_vol_cloud_sky.glsl).
    if (ssr.vol_clouds[0].x > 1.5) return pre;
    float cloud_scale = ssr.clouds[0].w;
    if (cloud_scale < 0.001) return pre;

    vec3 d = atmosToYUp(normalize(dir_world));
    // Ray from the camera (dome-local (0, H, 0)) to the dome sphere; H < R, so the
    // far root always exists.
    float b = CLOUD_CAM_HEIGHT * d.y;
    float t = -b + sqrt(b * b - (CLOUD_CAM_HEIGHT * CLOUD_CAM_HEIGHT - CLOUD_DOME_RADIUS * CLOUD_DOME_RADIUS));
    vec3  hit = vec3(0.0, CLOUD_CAM_HEIGHT, 0.0) + d * t;
    vec3  pn  = hit / CLOUD_DOME_RADIUS;
    if (pn.y < CLOUD_CAP_COS) return pre;   // outside LL's dome mesh

    // ---------------- cloudsV ----------------
    vec3  blue_horizon = ssr.atmos[0].rgb;  float haze_horizon = ssr.atmos[0].w;
    vec3  blue_density = ssr.atmos[1].rgb;  float haze_density = ssr.atmos[1].w;
    // (#783 M2: the weather's cloud cover, ssr.weather[11].z, only ever adds to the region's.)
    vec3  sunlight_color = ssr.atmos[2].rgb; float cloud_shadow = max(ssr.atmos[2].w, ssr.weather[11].z);
    vec3  ambient_color  = ssr.atmos[3].rgb; float max_y        = ssr.atmos[3].w;
    vec3  lightnorm    = atmosToYUp(ssr.atmos[4].xyz);
    float density_multiplier = ssr.atmos[4].w;
    vec3  glow         = ssr.atmos[5].xyz;
    float sun_moon_glow_factor = ssr.atmos[6].x;
    vec3  cloud_color  = ssr.clouds[0].rgb;
    // <GMDX> Physical night: the sky's own light (clean_atmos_common.glsl gmdxSkyLight), as the dome.
    if (gmdxNightOn())
    {
        sunlight_color = gmdxSkyLight(gmdxCloudMoonGain());   // (the moon's part x GMDXNightCloudLight)
        sun_moon_glow_factor = gmdxSkyGlowFactor();
    }

    // Planar dome texcoords (x/z transposed), flipped horizontally (SL-13084), scaled.
    vec2 tc0 = vec2((-pn.z + 1.0) * 0.5, (-pn.x + 1.0) * 0.5);
    tc0 = vec2(-tc0.x, tc0.y);
    tc0 = (tc0 - 0.5) / cloud_scale + 0.5;
    vec2 tc1 = tc0 + vec2(lightnorm.x, lightnorm.z) * 0.0125;
    vec2 tc2 = tc0 * 16.0;
    vec2 tc3 = tc1 * 16.0;

    vec3 rel_pos = d * t + vec3(0.0, 50.0, 0.0);
    float altitude_blend_factor = clamp((rel_pos.y + 512.0) / max_y, 0.0, 1.0);
    if (rel_pos.y > 0.0) rel_pos *= (max_y / rel_pos.y);
    if (rel_pos.y < 0.0)
    {
        altitude_blend_factor = 0.0;   // SL-11589 clouds drooping below the horizon
        rel_pos *= (-32000.0 / rel_pos.y);
    }
    vec3  rel_pos_norm = normalize(rel_pos);
    float rel_pos_len  = length(rel_pos);

    vec3 sunlight = sunlight_color;
    vec3 light_atten = (blue_density + vec3(haze_density * 0.25)) * (density_multiplier * max_y);
    vec3 combined_haze = max(abs(blue_density) + vec3(abs(haze_density)), vec3(1e-6));
    vec3 blue_weight = blue_density / combined_haze;
    vec3 haze_weight = haze_density / combined_haze;

    float off_axis = 1.0 / max(1e-6, max(0.0, rel_pos_norm.y) + lightnorm.y);
    sunlight *= exp(-light_atten * off_axis);

    float density_dist = rel_pos_len * density_multiplier;
    combined_haze = exp(-combined_haze * density_dist);

    float haze_glow = 1.0 - dot(rel_pos_norm, lightnorm);
    haze_glow = max(haze_glow, 0.001);
    haze_glow *= glow.x;
    haze_glow = pow(haze_glow, glow.z);
    haze_glow *= sun_moon_glow_factor;
    haze_glow = (sun_moon_glow_factor < 1.0) ? 0.0 : (haze_glow + 0.25);   // SL-13768

    vec3 tmpAmbient = ambient_color;
    tmpAmbient += (1.0 - tmpAmbient) * cloud_shadow * 0.5;
    tmpAmbient *= gmdxNightAmbientSrgb();   // <GMDX> the night's ambient (1 by day)
    sunlight *= (1.0 - cloud_shadow);

    vec3 additiveColorBelowCloud = blue_horizon * blue_weight * (sunlight + tmpAmbient)
                                 + (haze_horizon * haze_weight) * (sunlight * haze_glow + tmpAmbient);

    sunlight = sunlight_color;
    off_axis = 1.0 / max(1e-6, lightnorm.y * 2.0);
    sunlight *= exp(-light_atten * off_axis);

    vec3 cloudColorSun     = (sunlight * haze_glow) * cloud_color;
    vec3 cloudColorAmbient = tmpAmbient * cloud_color;

    combined_haze = sqrt(combined_haze);
    cloudColorSun     *= combined_haze;
    cloudColorAmbient *= combined_haze;
    cloudColorAmbient += additiveColorBelowCloud * (1.0 - combined_haze);

    float cloudDensity = 2.0 * (cloud_shadow - 0.25);

    // ---------------- cloudsF ----------------
    vec3  cloud_pos_density1 = ssr.clouds[1].xyz;
    float cloud_variance     = ssr.clouds[1].w;
    vec3  cloud_pos_density2 = ssr.clouds[2].xyz;

    vec2 uv1 = tc0, uv2 = tc1, uv3 = tc2, uv4 = tc3;

    vec2 disturbance  = vec2(cloudNoise(uv1 / 8.0).x, cloudNoise((uv3 + uv1) / 16.0).x)
                      * cloud_variance * (1.0 - cloud_scale * 0.25);
    vec2 disturbance2 = vec2(cloudNoise((uv1 + uv3) / 4.0).x, cloudNoise((uv4 + uv2) / 8.0).x)
                      * cloud_variance * (1.0 - cloud_scale * 0.25);

    uv1 += cloud_pos_density1.xy + (disturbance * 0.2);   // large texture, visible density
    uv2 += cloud_pos_density1.xy;                         // large texture, self shadow
    uv3 += cloud_pos_density2.xy;                         // small texture, visible density
    uv4 += cloud_pos_density2.xy;                         // small texture, self shadow

    float density_variance = min(1.0, (disturbance.x * 2.0 + disturbance.y * 2.0
                                       + disturbance2.x + disturbance2.y) * 4.0);
    cloudDensity *= 1.0 - (density_variance * density_variance);

    float alpha1 = (cloudNoise(uv1).x - 0.5) + (cloudNoise(uv3).x - 0.5) * cloud_pos_density2.z;
    alpha1 = min(max(alpha1 + cloudDensity, 0.0) * 10.0 * cloud_pos_density1.z, 1.0);
    alpha1 = 1.0 - alpha1 * alpha1;
    alpha1 = 1.0 - alpha1 * alpha1;
    alpha1 *= altitude_blend_factor;
    alpha1 *= 1.0 - gmdxSpace();   // <GMDX> space at altitude: the dome layer is always overhead; above the clouds it fades
    alpha1 = clamp(alpha1, 0.0, 1.0);

    float alpha2 = (cloudNoise(uv2).x - 0.5);
    alpha2 = min(max(alpha2 + cloudDensity, 0.0) * 2.5 * cloud_pos_density1.z, 1.0);
    alpha2 = 1.0 - alpha2;
    alpha2 = 1.0 - alpha2 * alpha2;

    vec3 color = cloudColorSun * (1.0 - alpha2) + cloudColorAmbient;
    color = clamp(color, vec3(0.0), vec3(1.0)) * 2.0;

    // <GMDX> Physical sky (clean_atmos_common.glsl): the same cloud shapes, lit by the physical
    // light (with physical night, the sun and the moon each) -- the sun through the air above the
    // cloud layer (transmittance LUT: white at noon, orange at sunset), forward scattering for
    // silver linings, the sky above as ambient -- and fading into the physical sky with the slant
    // distance to the layer.
    if (physkyOn())
    {
        const float layer_km = 1.5;
        vec3  dw = normalize(dir_world);
        float cloud_r = pkBottom() + max(pkViewHeight(), 0.0) + layer_km;
        // <GMDX> Physical night: a night sky's cloud colour below EEP's default (0.41) made up for
        // Second Life's full-strength moon; the moon's real light already darkens the clouds, so
        // once the sun has left the WindLight sky (sky_q < 1) the colour keeps its hue at the default's
        // brightness. By day as before.
        if (gmdxNightOn())
        {
            const float ccMax = max(max(cloud_color.r, cloud_color.g), max(cloud_color.b, 1e-3));
            cloud_color *= mix(1.0, max(1.0, 0.4099 / ccMax), 1.0 - clamp(ssr.vol_clouds[3].w, 0.0, 1.0));
        }
        vec3  lit =cloudPhysLit(pkLight(), pkLightDir(), dw, cloud_r, cloud_color, false);
        // Physical night: the moon is a second light (the one above is then the sun's).
        if (physkyMoonOn()) lit += cloudPhysLit(pkLight() * gmdxMoonSky() * gmdxCloudMoonGain(), physkyMoonDir(), dw, cloud_r, cloud_color, true);
        vec3  amb = physkySkyRaw(vec3(0.0, 0.0, 1.0)) * cloud_color * 1.2;
        vec3  c = lit * (1.0 - alpha2) + amb;
        // <GMDX> corona (GMDXCloudCorona), each light as above
        if (gcoOn())
        {
            c += cloudPhysCorona(pkLight(), pkLightDir(), dw, cloud_r, cloud_color, alpha1);
            if (physkyMoonOn()) c += cloudPhysCorona(pkLight() * gmdxMoonSky() * gmdxCloudMoonGain(), physkyMoonDir(), dw, cloud_r, cloud_color, alpha1);
        }
        float slant_km = layer_km / max(dw.z, 0.05);
        float tv = exp(-physkyExtinction() * slant_km * max(pkAerialScale() * 0.25, 1.0));
        c = c * tv + physkySkyRaw(dw) * (1.0 - tv);
        return mix(pre, physkyPre(physkyRolloff(c)), alpha1);
    }

    return mix(pre, color, alpha1);
}
