#version 450
// vkclean -- SMAA 1x pass 3 (neighbourhood blending) vertex stage.
#define SMAA_INCLUDE_VS 1
#define SMAA_INCLUDE_PS 0
#include "smaa_common.glsl"
#include "smaa_core.glsl"
layout(location = 0) out vec2 v_uv;
layout(location = 1) out vec4 v_off;
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    SMAANeighborhoodBlendingVS(v_uv, v_off);
}
