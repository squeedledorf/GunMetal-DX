// <GMDX> CLOUD CORONA: the glow and coloured rings thin clouds show around the moon and the sun.
//
// Settings GMDXCloudCorona (strength, 0 = off), GMDXCloudCoronaDroplet (droplet diameter, um) and
// GMDXCloudHalo22 (strength, 0 = off), frame info vol_clouds[3].xyz (LLCleanRendererVK fills it every frame, the volumetric layer live
// or not). Used by the volumetric march (gmdx_cloud_common.glsl vcMarch) and the physical sky's
// 2D cloud layer (clean_clouds.glsl), for each light. Both strengths 0: the clouds exactly as before.
//
// Physics, implemented from the published maths (no code taken from anywhere):
// - Fraunhofer diffraction by a droplet of diameter d (Babinet: an opaque disc diffracts like a
//   circular hole of the same size): intensity [2 J1(u) / u]^2, u = x sin(theta), x = pi d / lambda.
//   The integral of [2 J1(u) / u]^2 u du over (0, inf) is 2, so over the sphere the normalised
//   phase function is p = x^2 / (4 pi) [2 J1(u) / u]^2 per sr.
// - A large droplet takes twice its cross-section out of the beam (the extinction paradox): half
//   is this diffraction peak, so the peak carries half of what the droplet scatters (weight 0.5
//   at strength 1). The cloud model's own phase functions have no such peak (cloud renderers fold
//   it into the unscattered light), so it is added on top of them.
// - One wavelength per channel (R 610, G 540, B 460 nm): the rings scale with lambda / d, so the
//   colours run blue inside, red outside. Three droplet sizes (d x 0.88 / 1 / 1.12) blur the rings
//   as a real size spread does. The first red ring of d = 15 um is at u = 5.14:
//   5.14 lambda / (pi d) = 3.8 degrees. (Minnaert, "Light and Colour in the Outdoors", the corona
//   chapter; Laven, "Simulation of rainbows, coronas, and glories by use of Mie theory", Applied
//   Optics 42 (2003), and his corona pages at philiplaven.com: real coronae from 10 - 20 um droplets.)
// - Only single scattering keeps the angle: through a layer of optical depth tau in front of the
//   light the single-scattered light goes as tau exp(-tau), none in clear sky, faint behind thick
//   cloud, most at tau ~ 1. The march gets this by itself (each sample's light transmittance x the
//   view's); the 2D layer from its opacity, tau = -ln(1 - alpha).
// - Iridescence: the same diffraction where the droplets are small and alike. Where a cloud is
//   forming or evaporating (its thin edges) the droplets are smaller and their sizes narrower, so
//   the rings there are larger and more coloured, and as the cloud's thickness varies they break
//   into the pastel patches seen on thin cloud edges 10 - 20 degrees from the moon or sun (Minnaert;
//   Laven's iridescence pages). The callers pass k, the local droplet size as a fraction of the
//   setting's, from the cloud's own local density: k = 0.3 at the thinnest wisps, 1 in the body;
//   the size spread narrows with it (+-5 % at k = 0.3, +-12 % at 1). The first red ring of
//   4.5 um droplets lies at 13 degrees.
// - The 22-degree halo (optional: Second Life skies do not say whether a cloud is ice): light
//   refracted through the 60-degree prism of randomly turned hexagonal ice crystals. Rays bunch
//   at the minimum deviation D = 2 asin(n / 2) - 60 degrees, none inside it: a sharp inner edge
//   and a tail outward. Ice's index n = 1.3083 / 1.3105 / 1.3150 (R / G / B) gives D = 21.71 /
//   21.88 / 22.22 degrees: red inside. The profile is smoothstep(D - 0.35, D + 0.25) x
//   exp(-(theta - D) / 2.5 degrees) (the edge blurred by the light's own size and imperfect
//   crystals), scaled so the ring holds 10 % of the scattered light at strength 1 (the solid
//   angle of that profile is 0.1152 sr). (Minnaert, the halo chapter; Tape, "Atmospheric Halos",
//   1994; Greenler, "Rainbows, Halos, and Glories", 1980.)
// - J1: Abramowitz & Stegun, Handbook of Mathematical Functions, 9.4.4 (|u| <= 3) and 9.4.6
//   (u >= 3) polynomial approximations (error below 4e-8).
//
// Requires clean_frame_info.glsl (ssr).

#ifndef GMDX_CLOUD_CORONA_GLSL
#define GMDX_CLOUD_CORONA_GLSL

float gcoStrength() { return ssr.vol_clouds[3].x; }
float gcoDroplet()  { return ssr.vol_clouds[3].y; }
float gcoHalo()     { return ssr.vol_clouds[3].z; }
bool  gcoOn()       { return ssr.vol_clouds[3].x > 0.0 || ssr.vol_clouds[3].z > 0.0; }

// [2 J1(u) / u]^2, u >= 0 (1 at u = 0). From u = 10 to 20 its rings fade into their mean, the
// asymptote's 2 f^2 / u^3: a real spread of droplet sizes washes out the corona's higher orders
// (three sizes alone leave faint stripes out to 20 degrees).
float gcoAiry(float u)
{
    float j;   // J1(u) / u
    if (u < 3.0)
    {
        const float y = u * u * (1.0 / 9.0);
        j = 0.5 + y * (-0.56249985 + y * (0.21093573 + y * (-0.03954289 + y * (0.00443319
              + y * (-0.00031761 + y * 0.00001109)))));
    }
    else
    {
        const float y = 3.0 / u;
        const float f = 0.79788456 + y * (0.00000156 + y * (0.01659667 + y * (0.00017105
                      + y * (-0.00249511 + y * (0.00113653 - y * 0.00020033)))));
        const float th = u - 2.35619449 + y * (0.12499612 + y * (0.00005650 + y * (-0.00637879
                       + y * (0.00074348 + y * (0.00079824 - y * 0.00029166)))));
        const float cs = cos(th);
        return 4.0 * f * f / (u * u * u) * mix(cs * cs, 0.5, smoothstep(10.0, 20.0, u));
    }
    return 4.0 * j * j;
}

const vec3 GCO_LAMBDA = vec3(0.61, 0.54, 0.46);   // um, per channel

// Diffraction phase (per sr, rgb) of droplets of diameter d (um) +- spread at sin(theta) s.
vec3 gcoDiffraction(float s, float d, float spread)
{
    vec3 p = vec3(0.0);
    for (int i = -1; i <= 1; ++i)
    {
        const vec3 x = (3.14159265 * d * (1.0 + spread * float(i))) / GCO_LAMBDA;
        const vec3 u = x * s;
        p += (i == 0 ? 0.5 : 0.25) * x * x * vec3(gcoAiry(u.r), gcoAiry(u.g), gcoAiry(u.b));
    }
    return p * (1.0 / (4.0 * 3.14159265));
}

// Within 30 degrees of the light from L seen along V (both unit; V from the eye, so dot(V, L) = 1
// looks straight at the light), with the corona on; 36 with the halo on.
bool gcoNear(vec3 V, vec3 L) { return gcoOn() && dot(V, L) > (gcoHalo() > 0.0 ? 0.809 : 0.866); }

// The local droplet size (a fraction of GMDXCloudCoronaDroplet) from how far the cloud there is
// from its thinnest (0) to its body (1).
float gcoDropletScale(float body01) { return mix(0.3, 1.0, clamp(body01, 0.0, 1.0)); }

// The corona's (and the halo's) phase (per sr, rgb) for light from L seen along V, droplets k x
// the setting's size. The corona: 0 when off or more than 30 degrees from the light (faded from
// 20: nothing of it shows that far); the halo: 0 when off or past 36 degrees.
vec3 gcoPhase(vec3 V, vec3 L, float k)
{
    const float c = dot(V, L);
    const float s = length(cross(V, L));
    vec3 p = vec3(0.0);
    if (gcoStrength() > 0.0 && c > 0.866)
        p = gcoDiffraction(s, gcoDroplet() * k, mix(0.05, 0.12, (k - 0.3) / 0.7))
          * (0.5 * gcoStrength() * smoothstep(0.866, 0.940, c));
    if (gcoHalo() > 0.0 && c > 0.809)
    {
        const vec3 th = vec3(degrees(atan(s, c)));
        const vec3 D = vec3(21.71, 21.88, 22.22);
        p += smoothstep(D - 0.35, D + 0.25, th) * exp(-max(th - D, vec3(0.0)) * (1.0 / 2.5))
           * (0.1 / 0.1152 * gcoHalo() * smoothstep(0.809, 0.829, c));
    }
    return p;
}

// Single-scattering albedo from EEP's cloud colour, as the volumetric layer's with physical
// night: 0.9 in the colour's hue, darker below the default colour (0.41).
vec3 gcoAlbedo(vec3 cloud_color)
{
    const vec3 cc = max(cloud_color, vec3(1e-3));
    const float m = max(max(cc.r, cc.g), cc.b);
    return 0.9 * cc / m * min(m / 0.4099, 1.0);
}

#endif
