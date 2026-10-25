#version 450
// vkclean -- SMAA 1x pass 2: blending weights from the edges + area/search LUTs.
#define SMAA_INCLUDE_VS 0
#define SMAA_INCLUDE_PS 1
#include "smaa_common.glsl"
#include "smaa_core.glsl"
layout(set = 0, binding = 0) uniform sampler2D edgesTex;
layout(set = 0, binding = 1) uniform sampler2D areaTex;
layout(set = 0, binding = 2) uniform sampler2D searchTex;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec2 v_pix;
layout(location = 2) in vec4 v_off[3];
layout(location = 0) out vec4 outColor;
void main()
{
    outColor = SMAABlendingWeightCalculationPS(v_uv, v_pix, v_off,
                                               edgesTex, areaTex, searchTex, vec4(0.0));
}
