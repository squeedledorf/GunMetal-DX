#version 450
// vkclean — cross (star) filter streaks (Lightbox; port of upstream Alchemy
// class1/effects/crossFilterF.glsl). One arm, one direction, one-sided: three
// passes per arm with the step quadrupling (1, 4, 16) compose to an exact
// exponential line filter over 0..63 texels. Streaks are drawn at half the bloom
// base resolution; arms accumulate additively (a separate pipeline).
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uSrc;

layout(push_constant) uniform CrossPush {
    vec4 p0;   // texel.xy (streak target), dir.xy
    vec4 p1;   // length, falloff, chromatic, pass scale
} pc;

const int   TAPS        = 4;
const float CHAIN_REACH = float(TAPS * TAPS * TAPS - 1);

vec3 spectrum(float t)
{
    float s = clamp(t, 0.0, 1.0) * 3.0;
    return clamp(vec3(1.5 - abs(s - 0.5), 1.5 - abs(s - 1.5), 1.5 - abs(s - 2.5)), 0.0, 1.0);
}

void main()
{
    vec3 accum = vec3(0.0);
    float total_w = 0.0;
    for (int i = 0; i < TAPS; ++i)
    {
        float step_index = float(i) * pc.p1.w;
        vec2 offset = pc.p0.zw * pc.p0.xy * pc.p1.x * step_index;
        float weight = pow(pc.p1.y, -step_index);
        vec3 tint = vec3(1.0);
        if (pc.p1.z > 0.0)
        {
            float t = clamp(step_index / CHAIN_REACH, 0.0, 1.0);
            vec3 sp = spectrum(t);
            sp *= 3.0 / max(sp.r + sp.g + sp.b, 1e-4);
            tint = mix(vec3(1.0), sp, pc.p1.z * t);
        }
        accum += texture(uSrc, clamp(v_uv + offset, 0.0, 1.0)).rgb * weight * tint;
        total_w += weight;
    }
    outColor = vec4(accum / max(total_w, 1e-4), 0.0);
}
