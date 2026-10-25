#version 450
// vkclean — DoF pass 1 (LL cofF.glsl): scene colour + signed circle of confusion.
#include "clean_dof_common.glsl"
layout(set = 0, binding = 0) uniform sampler2D uColor;   // tonemapped scene (NEAREST)
layout(set = 0, binding = 1) uniform sampler2D uDepth;   // world depth (NEAREST)

float calc_cof(float depth)
{
    const float focal_distance = pc.lens.x, blur_constant = pc.lens.y;
    const float tan_pixel_angle = pc.lens.z, magnification = pc.lens.w;
    float sc = (depth - focal_distance) / -depth * blur_constant;
    sc /= magnification;
    // tan_pixel_angle = pixel_length/-depth;
    float pixel_length = tan_pixel_angle * -focal_distance;
    sc = sc / pixel_length;
    sc *= 1.414;
    return sc;
}

void main()
{
    float z = texture(uDepth, v_uv).r;
    vec4 p = pc.inv_proj * vec4(0.0, 0.0, z, 1.0);   // VK NDC z = depth
    float depth = p.z / p.w;
    vec4 diff = texture(uColor, v_uv);
    const float max_cof = pc.params.x;
    float sc = clamp(calc_cof(depth), -max_cof, max_cof);
    outColor = vec4(diff.rgb, sc / max_cof * 0.5 + 0.5);
}
