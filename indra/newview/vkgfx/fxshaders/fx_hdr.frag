#version 450
// Slipstream FX host (#548): HDR injection point in / out. The renderer's linear scene
// (mHdr, RGBA16F, pre-exposure) <-> the HDR-point effects' back buffer, exposure-scaled
// so effects see the values the tonemapper will see (> 1 allowed). FX_HDR_OUT divides
// the exposure back out, so the tonemapper's own exposure still applies exactly once.
// The exposure is the renderer's 1x1 R32F (GENERAL storage image, imageLoad) times a
// constant (push); without dynamic exposure the image term is skipped (use_img = 0).
layout(set = 0, binding = 0) uniform sampler2D uSrc;
layout(set = 0, binding = 1, r32f) uniform readonly image2D uExposure;
layout(push_constant) uniform Push {
    float exposure_const;
    float use_img;
    float pad0, pad1;
} pc;
layout(location = 0) out vec4 o_color;

void main()
{
    float e = pc.exposure_const;
    if (pc.use_img > 0.5) e *= imageLoad(uExposure, ivec2(0)).r;
    e = max(e, 1e-6);
    const vec4 c = texelFetch(uSrc, ivec2(gl_FragCoord.xy), 0);
#if defined(FX_HDR_OUT)
    o_color = vec4(c.rgb / e, c.a);
#else
    o_color = vec4(c.rgb * e, c.a);
#endif
}
