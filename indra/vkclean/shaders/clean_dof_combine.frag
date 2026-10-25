#version 450
// vkclean — DoF pass 3 (LL dofCombineF.glsl): blend the reduced-resolution DoF
// result over the sharp image by the circle of confusion. Writes the linear HDR
// scene in place (colour only: the pipeline blend keeps the target's alpha).
#include "clean_dof_common.glsl"
layout(set = 0, binding = 0) uniform sampler2D uDof;     // gather result (LINEAR upsample)
layout(set = 0, binding = 1) uniform sampler2D uCoC;     // colour + CoC (NEAREST)

void main()
{
    const float max_cof = pc.params.x, res_scale = pc.params.y;
    vec4 dof  = texture(uDof, v_uv);
    vec4 diff = texture(uCoC, v_uv);
    // Without near blur (RenderDepthOfFieldNearBlur off, the upstream default) the
    // foreground (CoC above the midpoint) stays sharp.
    float a = (pc.params.z <= 0.5 && diff.a > 0.5) ? 0.0
            : min(abs(diff.a * 2.0 - 1.0) * max_cof * res_scale * res_scale, 1.0);
    if (a > 0.25 && a < 0.75)
    {   // help out the transition a bit
        float sc = a / res_scale;
        vec4 col;
        col  = texture(uCoC, v_uv + vec2( sc,  sc) * pc.screen.zw);
        col += texture(uCoC, v_uv + vec2(-sc,  sc) * pc.screen.zw);
        col += texture(uCoC, v_uv + vec2( sc, -sc) * pc.screen.zw);
        col += texture(uCoC, v_uv + vec2(-sc, -sc) * pc.screen.zw);
        diff = mix(diff, col * 0.25, a);
    }
    outColor = vec4(mix(diff, dof, a).rgb, 1.0);
}
