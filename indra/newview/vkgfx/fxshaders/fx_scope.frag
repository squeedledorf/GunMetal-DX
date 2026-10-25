#version 450
// Slipstream FX host (#548): per-effect SCOPE. The host saved the image before the
// technique ran (uPre) and blends the technique's result (uPost) back by the resolved
// avatar mask: mode 1 = avatars only, 2 = everything except avatars. The mask is the
// display-res, unjittered avatar id (R32F: 0 = no avatar). Feather = a box of
// 2r+1 x 2r+1 display pixels (r = pc.feather, 0..8): the coverage fraction softens the
// edge instead of a hard id cut.
layout(set = 0, binding = 0) uniform sampler2D uPre;
layout(set = 0, binding = 1) uniform sampler2D uPost;
layout(set = 0, binding = 2) uniform sampler2D uMask;
layout(push_constant) uniform Push {
    int mode;      // 1 avatars only, 2 except avatars
    int feather;   // box radius in display pixels
    int pad0, pad1;
} pc;
layout(location = 0) out vec4 o_color;

void main()
{
    const ivec2 p = ivec2(gl_FragCoord.xy);
    const ivec2 sz = textureSize(uMask, 0);
    const int r = clamp(pc.feather, 0, 8);
    float cover = 0.0;
    float n = 0.0;
    for (int y = -r; y <= r; ++y)
        for (int x = -r; x <= r; ++x)
        {
            const ivec2 q = clamp(p + ivec2(x, y), ivec2(0), sz - 1);
            cover += texelFetch(uMask, q, 0).r > 0.5 ? 1.0 : 0.0;
            n += 1.0;
        }
    float w = cover / max(n, 1.0);
    if (pc.mode == 2) w = 1.0 - w;
    const vec4 pre = texelFetch(uPre, p, 0);
    const vec4 post = texelFetch(uPost, p, 0);
    o_color = mix(pre, post, w);
}
