#version 450
// vkclean — glow composite (LL glowcombineF: scene + glow). Drawn with additive
// blending (ONE, ONE) onto the swapchain after the world passes, before HUDs.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D uGlow;

void main()
{
    outColor = vec4(texture(uGlow, v_uv).rgb, 0.0);
}
