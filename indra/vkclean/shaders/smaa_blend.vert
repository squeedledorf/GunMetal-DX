#version 450
// vkclean -- SMAA 1x pass 2 (blending weights) vertex stage (preset-dependent:
// SMAA_MAX_SEARCH_STEPS sizes the search offsets).
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 0
#include "smaa_common.glsl"
#include "smaa_core.glsl"
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec2 v_pix;
layout(location = 2) out vec4 v_off[3];
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    SMAABlendingWeightCalculationVS(v_uv, v_pix, v_off);
}
