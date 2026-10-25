#version 450
// vkclean — AVATAR ID TARGET consumers (#549, llcelshadevk.cpp), fullscreen triangle
// (clean_glow_combine.vert), texelFetch only (ids never interpolate).
//
// ID_RESOLVE: the DISPLAY-resolution, UNJITTERED copy of the render-res id target for
//   the display-space consumers (the FX stack's avatar mask). Same mapping as
//   clean_depth_up.comp: render pixel q shows the scene at q + 0.5 - jitter, so the one
//   covering render position pr is floor(pr + jitter).
// default (debug view, RenderVKDebugView 22 / RenderVKCelDebugMask): the ids as colours
//   over the scene HDR (blended; 0 = no avatar darkens).
layout(set = 0, binding = 0) uniform usampler2D uId;   // render-res avatar slot + 1

#ifdef ID_RESOLVE
layout(location = 0) out uint outId;
layout(push_constant) uniform Push {
    ivec2 dsize;    // display extent
    ivec2 rsize;    // render extent
    vec2  ratio;    // render / display
    vec2  jitter;   // this frame's jitter, render pixels (x right, y down)
} pc;

void main()
{
    const ivec2 p  = ivec2(gl_FragCoord.xy);
    const vec2  pr = (vec2(p) + 0.5) * pc.ratio;
    const ivec2 q  = clamp(ivec2(floor(pr + pc.jitter)), ivec2(0), pc.rsize - 1);
    outId = texelFetch(uId, q, 0).r;
}
#else
layout(location = 0) out vec4 outColor;

void main()
{
    const uint id = texelFetch(uId, ivec2(gl_FragCoord.xy), 0).r;
    if (id == 0u)
    {
        outColor = vec4(0.0, 0.0, 0.0, 0.65);
        return;
    }
    const float hue = fract(float(id) * 0.61803399);
    const vec3 rgb = clamp(abs(fract(hue + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
    outColor = vec4(rgb, 0.85);
}
#endif
