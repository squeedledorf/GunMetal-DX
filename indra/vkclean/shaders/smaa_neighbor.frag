#version 450
// vkclean -- SMAA 1x pass 3: blend each pixel with its neighbours by the weights,
// writing the anti-aliased gamma-encoded image to the swapchain.
#define SMAA_INCLUDE_VS 0
#define SMAA_INCLUDE_PS 1
#include "smaa_common.glsl"
#include "smaa_core.glsl"
layout(set = 0, binding = 0) uniform sampler2D colorTex;
layout(set = 0, binding = 1) uniform sampler2D blendTex;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_off;
layout(location = 0) out vec4 outColor;
// LL's SMAA.glsl blends in linear space (the input is gamma-encoded): exact sRGB
// transfer functions, as clean_tonemap.frag's linear_to_srgb.
vec3 srgb_to_linear(vec3 cs)
{
    vec3 low  = cs / 12.92;
    vec3 high = pow((cs + 0.055) / 1.055, vec3(2.4));
    return mix(high, low, lessThanEqual(cs, vec3(0.04045)));
}
vec4 srgb_to_linear4(vec4 cs) { return vec4(srgb_to_linear(cs.rgb), cs.a); }
vec3 linear_to_srgb(vec3 cl)
{
    cl = clamp(cl, vec3(0.0), vec3(1.0));
    vec3 low  = cl * 12.92;
    vec3 high = 1.055 * pow(cl, vec3(1.0 / 2.4)) - 0.055;
    return mix(high, low, lessThan(cl, vec3(0.0031308)));
}

void main()
{
    outColor = vec4(SMAANeighborhoodBlendingPS(v_uv, v_off, colorTex, blendTex).rgb, 1.0);
}
