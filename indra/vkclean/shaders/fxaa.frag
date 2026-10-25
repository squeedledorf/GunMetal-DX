#version 450
// vkclean -- FXAA 3.11 (PC quality) on the gamma-encoded LDR image -> swapchain.
// Luma from green (no separate luma bake pass). Preset from the regen script's
// -DFXAA_QUALITY__PRESET=12/23/28/39 (LL: Low/Medium/High/Ultra). Edge settings
// as LL's fxaaF.glsl.
#define FXAA_PC 1
#define FXAA_GLSL_400 1
#define FXAA_GREEN_AS_LUMA 1
#include "fxaa_core.glsl"
layout(push_constant) uniform FxaaPush { vec4 rcp; } pc;   // xy = 1/size
layout(set = 0, binding = 0) uniform sampler2D colorTex;
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;
void main()
{
    vec4 c = FxaaPixelShader(v_uv, vec4(0.0), colorTex, colorTex, colorTex,
                             pc.rcp.xy, vec4(0.0), vec4(0.0), vec4(0.0),
                             0.75, 0.07, 0.03, 8.0, 0.125, 0.05, vec4(0.0));
    outColor = vec4(c.rgb, 1.0);
}
