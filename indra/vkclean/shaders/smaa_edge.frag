#version 450
// vkclean -- SMAA 1x pass 1: colour edge detection on the gamma-encoded LDR image
// (as LL: SMAAColorEdgeDetectionPS). Discards pixels without edges; the edge
// target is cleared to 0.
#define SMAA_INCLUDE_VS 0
#define SMAA_INCLUDE_PS 1
#include "smaa_common.glsl"
#include "smaa_core.glsl"
layout(set = 0, binding = 0) uniform sampler2D colorTex;
layout(location = 0) in vec2 v_uv;
layout(location = 1) in vec4 v_off[3];
layout(location = 0) out vec4 outColor;
void main()
{
    outColor = vec4(SMAAColorEdgeDetectionPS(v_uv, v_off, colorTex), 0.0, 0.0);
}
