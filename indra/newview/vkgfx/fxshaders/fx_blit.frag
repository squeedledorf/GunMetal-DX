#version 450
// Slipstream FX host (#544): copy the effects' result into the post chain's own image
// (mFinalColor / the RLVa sphere ping-pong target): same size, texel for texel.
layout(set = 0, binding = 0) uniform sampler2D uSrc;
layout(location = 0) out vec4 o_color;

void main()
{
    o_color = texelFetch(uSrc, ivec2(gl_FragCoord.xy), 0);
}
