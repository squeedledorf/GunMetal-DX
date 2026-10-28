// <GMDX> PHYSICAL SKY maths (RenderVKPhysicalSky; the C++ half is newview/vkgfx/gmdxphysskyvk.cpp).
//
// Hillaire 2020, "A Scalable and Production Ready Sky and Atmosphere Rendering Technique"
// (EGSR 2020). Adapted from Epic Games' reference implementation,
// https://github.com/sebh/UnrealEngineSkyAtmosphere, MIT License,
// Copyright (c) 2020 Epic Games, Inc.:
//   Permission is hereby granted, free of charge, to any person obtaining a copy of this
//   software and associated documentation files (the "Software"), to deal in the Software
//   without restriction, including without limitation the rights to use, copy, modify, merge,
//   publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons
//   to whom the Software is furnished to do so, subject to the following conditions: The above
//   copyright notice and this permission notice shall be included in all copies or substantial
//   portions of the Software. THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
//   EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
//   FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
//   HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
//   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE
//   USE OR OTHER DEALINGS IN THE SOFTWARE.
//
// Units are kilometres; the frame is Z up with the planet centre at the origin, so a viewer
// at height h stands at (0, 0, bottom + h) and world directions (Z up) are used as they are.
//
// The includer defines PHYSKY_P(i): the i-th parameter vec4 (frame info physky[], or the LUT
// pass's push block), filled by LLCleanRendererVK::physSkyFill from the EEP sky:
//   [0] Rayleigh scattering rgb (1/km), bottom radius     [1] Mie scattering rgb, top radius
//   [2] Mie extinction rgb, Mie g                          [3] light illuminance rgb (linear,
//                                                              exposure applied), viewer height
//   [4] light direction (world, unit), on                  [5] ambient rgb (linear), Rayleigh
//                                                              exp scale (1/km, negative)
//       (<GMDX> physical night: [3] / [4] are the sun's; [4].w 2 = the moon is a second light,
//       3 = the sky-view pass of the moon's half; gmdxphysskyvk.cpp)
//   [6] Mie exp scale (1/km, negative), ground albedo, highlight roll-off (0 none), aerial
//       perspective distance scale (0 = WindLight's haze only)
//   [7] Rayleigh in-scatter tint rgb (EEP blue_horizon), Mie in-scatter tint (haze_horizon):
//       the sky's colour as its artist set it, on what is scattered, not on what is absorbed

#define PK_PI 3.14159265
#define PK_TRANS_W 256.0
#define PK_TRANS_H 64.0
#define PK_MS_RES 32.0
#define PK_SV_W 192.0
#define PK_SV_H 108.0
#define PK_PLANET_OFFSET 0.01

float pkBottom()       { return PHYSKY_P(0).w; }
float pkTop()          { return PHYSKY_P(1).w; }
vec3  pkRayleigh()     { return PHYSKY_P(0).rgb; }
vec3  pkMieScat()      { return PHYSKY_P(1).rgb; }
vec3  pkMieExt()       { return PHYSKY_P(2).rgb; }
float pkMieG()         { return PHYSKY_P(2).w; }
vec3  pkLight()        { return PHYSKY_P(3).rgb; }
float pkViewHeight()   { return PHYSKY_P(3).w; }
vec3  pkLightDir()     { return PHYSKY_P(4).xyz; }
vec3  pkAmbient()      { return PHYSKY_P(5).rgb; }
float pkRayleighExp()  { return PHYSKY_P(5).w; }
float pkMieExp()       { return PHYSKY_P(6).x; }
float pkGroundAlbedo() { return PHYSKY_P(6).y; }
float pkAerialScale()  { return PHYSKY_P(6).w; }
vec3  pkRayleighTint() { return PHYSKY_P(7).rgb; }
float pkMieTint()      { return PHYSKY_P(7).w; }

// Earth's ozone layer (Bruneton's two linear layers), not in EEP's legacy haze: constant.
const vec3 PK_OZONE = vec3(0.650e-3, 1.881e-3, 0.085e-3);

// Distance along rd from r0 to the nearest hit of the sphere of radius R at the origin, or -1.
float pkRaySphere(vec3 r0, vec3 rd, float R)
{
    float b = 2.0 * dot(rd, r0);
    float c = dot(r0, r0) - R * R;
    float delta = b * b - 4.0 * c;
    if (delta < 0.0) return -1.0;
    float s = sqrt(delta);
    float sol0 = (-b - s) * 0.5;
    float sol1 = (-b + s) * 0.5;
    if (sol0 < 0.0 && sol1 < 0.0) return -1.0;
    if (sol0 < 0.0) return max(0.0, sol1);
    if (sol1 < 0.0) return max(0.0, sol0);
    return max(0.0, min(sol0, sol1));
}

float pkUnitToSub(float u, float res) { return (u + 0.5 / res) * (res / (res + 1.0)); }
float pkSubToUnit(float u, float res) { return (u - 0.5 / res) * (res / (res - 1.0)); }

// Transmittance LUT parameterisation (Bruneton 2017).
vec2 pkTransUv(float viewHeight, float viewZenithCos)
{
    float B = pkBottom(), T = pkTop();
    float H = sqrt(max(0.0, T * T - B * B));
    float rho = sqrt(max(0.0, viewHeight * viewHeight - B * B));
    float disc = viewHeight * viewHeight * (viewZenithCos * viewZenithCos - 1.0) + T * T;
    float d = max(0.0, -viewHeight * viewZenithCos + sqrt(max(disc, 0.0)));
    float d_min = T - viewHeight;
    float d_max = rho + H;
    return vec2((d - d_min) / max(d_max - d_min, 1e-6), rho / max(H, 1e-6));
}

void pkTransParams(vec2 uv, out float viewHeight, out float viewZenithCos)
{
    float B = pkBottom(), T = pkTop();
    float H = sqrt(max(0.0, T * T - B * B));
    float rho = H * uv.y;
    viewHeight = sqrt(rho * rho + B * B);
    float d_min = T - viewHeight;
    float d_max = rho + H;
    float d = d_min + uv.x * (d_max - d_min);
    viewZenithCos = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * viewHeight * d);
    viewZenithCos = clamp(viewZenithCos, -1.0, 1.0);
}

// Sky-view LUT parameterisation (non-linear around the horizon).
vec2 pkSkyViewUv(bool intersectGround, float viewZenithCos, float lightViewCos, float viewHeight)
{
    float B = pkBottom();
    float vhorizon = sqrt(max(viewHeight * viewHeight - B * B, 0.0));
    float beta = acos(clamp(vhorizon / viewHeight, -1.0, 1.0));
    float zenithHorizon = PK_PI - beta;
    vec2 uv;
    if (!intersectGround)
    {
        float coord = acos(clamp(viewZenithCos, -1.0, 1.0)) / zenithHorizon;
        coord = 1.0 - sqrt(max(1.0 - coord, 0.0));
        uv.y = coord * 0.5;
    }
    else
    {
        float coord = (acos(clamp(viewZenithCos, -1.0, 1.0)) - zenithHorizon) / max(beta, 1e-6);
        uv.y = sqrt(max(coord, 0.0)) * 0.5 + 0.5;
    }
    uv.x = sqrt(clamp(-lightViewCos * 0.5 + 0.5, 0.0, 1.0));
    return vec2(pkUnitToSub(uv.x, PK_SV_W), pkUnitToSub(uv.y, PK_SV_H));
}

void pkSkyViewParams(vec2 uv, float viewHeight, out float viewZenithCos, out float lightViewCos)
{
    uv = vec2(pkSubToUnit(uv.x, PK_SV_W), pkSubToUnit(uv.y, PK_SV_H));
    float B = pkBottom();
    float vhorizon = sqrt(max(viewHeight * viewHeight - B * B, 0.0));
    float beta = acos(clamp(vhorizon / viewHeight, -1.0, 1.0));
    float zenithHorizon = PK_PI - beta;
    if (uv.y < 0.5)
    {
        float coord = 1.0 - 2.0 * uv.y;
        coord = 1.0 - coord * coord;
        viewZenithCos = cos(zenithHorizon * coord);
    }
    else
    {
        float coord = uv.y * 2.0 - 1.0;
        viewZenithCos = cos(zenithHorizon + beta * coord * coord);
    }
    float c = uv.x * uv.x;
    lightViewCos = -(c * 2.0 - 1.0);
}

struct PkMedium
{
    vec3 scattering;
    vec3 extinction;
    vec3 scatMie;
    vec3 scatRay;
};

PkMedium pkMedium(vec3 p)
{
    float h = length(p) - pkBottom();
    float dMie = exp(pkMieExp() * h);
    float dRay = exp(pkRayleighExp() * h);
    float dOzo = clamp(h < 25.0 ? h / 15.0 - 2.0 / 3.0 : -h / 15.0 + 8.0 / 3.0, 0.0, 1.0);
    PkMedium m;
    m.scatMie = dMie * pkMieScat() * pkMieTint();
    m.scatRay = dRay * pkRayleigh() * pkRayleighTint();
    m.scattering = m.scatMie + m.scatRay;
    m.extinction = dMie * pkMieExt() + dRay * pkRayleigh() + dOzo * PK_OZONE;   // (untinted)
    return m;
}

float pkRayleighPhase(float cosTheta) { return 3.0 / (16.0 * PK_PI) * (1.0 + cosTheta * cosTheta); }

// Cornette-Shanks, peaking toward the light (cosTheta = dot(light, view)).
float pkMiePhase(float g, float cosTheta)
{
    float k = 3.0 / (8.0 * PK_PI) * (1.0 - g * g) / (2.0 + g * g);
    return k * (1.0 + cosTheta * cosTheta) / pow(max(1.0 + g * g - 2.0 * g * cosTheta, 1e-4), 1.5);
}
