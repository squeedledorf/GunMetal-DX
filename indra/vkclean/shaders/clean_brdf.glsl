// vkclean -- shared shading terms: the world lit FS (clean_draw_common.glsl) and the
// ray-traced hit shading (clean_rt_shade.glsl, compute) use the SAME functions, so a
// ray-traced reflection-probe face lights a surface exactly as the raster face does.
// Stage-agnostic (no derivatives, no bindings).
#ifndef CLEAN_BRDF_GLSL
#define CLEAN_BRDF_GLSL

// Linear -> sRGB (the stored/display encoding of an sRGB texture).
vec3 srgbEncode(vec3 c)
{
    c = clamp(c, vec3(0.0), vec3(1.0));
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0/2.4)) - 0.055, step(vec3(0.0031308), c));
}

const float PI = 3.14159265;

float D_GGX(float NoH, float a)
{
    float a2 = a * a;
    float d  = NoH * NoH * (a2 - 1.0) + 1.0;
    return a2 / (PI * d * d);
}

float V_SmithGGXCorrelated(float NoV, float NoL, float a)
{
    float a2 = a * a;
    float gv = NoL * sqrt(NoV * NoV * (1.0 - a2) + a2);
    float gl = NoV * sqrt(NoL * NoL * (1.0 - a2) + a2);
    return 0.5 / max(gv + gl, 1e-5);
}

vec3 F_Schlick(vec3 F0, float c) { return F0 + (1.0 - F0) * pow(1.0 - c, 5.0); }

// Split-sum environment BRDF approximation (Karis, mobile).
vec3 EnvBRDFApprox(vec3 F0, float r, float NoV)
{
    const vec4 c0 = vec4(-1.0, -0.0275, -0.572, 0.022);
    const vec4 c1 = vec4( 1.0,  0.0425,  1.04, -0.04);
    vec4 rr = r * c0 + c1;
    float a004 = min(rr.x * rr.x, exp2(-9.28 * NoV)) * rr.x + rr.y;
    vec2 AB = vec2(-1.04, 1.04) * a004 + rr.zw;
    return F0 * AB.x + AB.y;
}

// LL pbrPunctual (glTF reference BRDF: Lambert / PI, Smith-Schlick G, GGX D).
void llPunctual(vec3 diffuseColor, vec3 specularColor, float perceptualRoughness,
                vec3 n, vec3 v, vec3 l, out float nl, out vec3 diff, out vec3 spec)
{
    perceptualRoughness = max(perceptualRoughness, 8.0 / 255.0);
    float r  = perceptualRoughness * perceptualRoughness;
    float r2 = r * r;
    float reflectance = max(max(specularColor.r, specularColor.g), specularColor.b);
    vec3  R90 = vec3(clamp(reflectance * 25.0, 0.0, 1.0));
    vec3  h = normalize(l + v);
    float NdotL = clamp(dot(n, l), 0.001, 1.0);
    float NdotV = clamp(abs(dot(n, v)), 0.001, 1.0);
    float NdotH = clamp(dot(n, h), 0.0, 1.0);
    float VdotH = clamp(dot(v, h), 0.0, 1.0);
    vec3  F  = specularColor + (R90 - specularColor) * pow(clamp(1.0 - VdotH, 0.0, 1.0), 5.0);
    float gL = 2.0 * NdotL / (NdotL + sqrt(r2 + (1.0 - r2) * NdotL * NdotL));
    float gV = 2.0 * NdotV / (NdotV + sqrt(r2 + (1.0 - r2) * NdotV * NdotV));
    float f  = (NdotH * r2 - NdotH) * NdotH + 1.0;
    float D  = r2 / (PI * f * f);
    diff = (1.0 - F) * diffuseColor / PI;
    spec = F * gL * gV * D / (4.0 * NdotL * NdotV);
    nl = NdotL;
}

// ---- LL LEGACY (Blinn-Phong) SPECULAR, shared by the sun term (softenLightF / materialF),
// the local lights (multiPointLightF / spotLightF / materialF calcPointLightOrSpotLight)
// and the ray-traced hit shading, so the four can't drift (#470).
//
// LL's lightFunc LUT (pipeline.cpp createLUTBuffers, R16F 1024 x 256, sampled at
// (nh, glossiness)) evaluated analytically: normalized Blinn-Phong with
// n = glossiness^2 * RenderSpecularExponent (368).
float legacyLightFunc(float nh, float gloss)
{
    const float n = gloss * gloss * 368.0;
    return pow(nh, n) * ((n + 2.0) * (n + 4.0)) / (8.0 * PI * (exp2(-n * 0.5) + n));
}

// The per-light legacy specular factor: fres * lightFunc(nh, gloss) * gt / (nh * nl),
// with LL's calcHalfVectors clamps (1e-6). `lit` = LL's min(nl * 6, 1) ramp. Both 0
// where the surface faces away from the light (LL's clamped nl leaves only a 6e-6 ramp
// there). Multiply by the light colour (x attenuation / shadow) and the linear specular
// colour.
float legacySpecular(vec3 N, vec3 V, vec3 l, float gloss, out float lit)
{
    lit = 0.0;
    const vec3 hv = normalize(l + V);
    // (!(x > 0) so a NaN half vector -- l == -V -- is rejected too)
    if (!(gloss > 0.0) || !(dot(N, l) > 0.0) || !(dot(N, hv) > 0.0)) return 0.0;
    const float nh = clamp(dot(N, hv), 1e-6, 1.0);
    const float nl = clamp(dot(N, l),  1e-6, 1.0);
    const float nv = clamp(dot(N, V),  1e-6, 1.0);
    const float vh = clamp(dot(V, hv), 1e-6, 1.0);
    lit = min(nl * 6.0, 1.0);
    const float fres = pow(1.0 - vh, 5.0) * 0.4 + 0.5;
    const float gtd  = 2.0 * nh;
    const float gt   = max(0.0, min(gtd * nv / vh, gtd * nl / vh));
    return fres * legacyLightFunc(nh, gloss) * gt / (nh * nl);
}

// A legacy face's specular inputs for the local-light loop: linear specular colour,
// glossiness (specular exponent x normal-map alpha), env intensity (projector image
// reflection). gloss 0 and env 0 = no legacy specular (PBR, particles, matte faces).
struct LegacySpec { vec3 spec; float gloss; float env; };
LegacySpec legacySpecNone() { LegacySpec ls; ls.spec = vec3(0.0); ls.gloss = 0.0; ls.env = 0.0; return ls; }

#endif
