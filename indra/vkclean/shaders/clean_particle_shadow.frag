#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
// vkclean — GPU particles as SUN SHADOW casters (#39). Drawn with clean_particle.vert
// (billboards turned to face the light: the recorder passes the cascade's basis as
// u_view) into the shadow cascades, light-blocking "over" blend pairs only (alpha,
// premultiplied alpha: additive / modulate sprites add or tint light, not block it).
//   (default)     depth-only cascade, alpha-tested at 0.5 like the alpha-blend world
//                 casters (clean_shadow_mask.frag): soft sprites cast their shape, not
//                 their quad;
//   TRANS_DEPTH   translucency layer depth (#212): nearest translucent caster;
//   TRANS_COLOR   translucency layer colour: dst *= the sprite's transmittance
//                 (clean_translucent.glsl), so smoke throws a partial, tinted shadow.
// Inputs match clean_particle.vert.
#include "clean_translucent.glsl"
layout(location = 0) flat in uint v_tex;   // bindless index (0xFFFFFFFF = not resident)
layout(location = 1)      in vec2 v_uv;
layout(location = 3) flat in vec4 v_tint;  // sRGB colour x alpha

layout(set = 1, binding = 0) uniform sampler2D uTextures[];

#ifdef TRANS_COLOR
layout(location = 0) out vec4 outT;
#endif

vec3 srgbToLinear(vec3 c)
{
    c = clamp(c, 0.0, 1.0);
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(vec3(0.04045), c));
}

void main()
{
    if (v_tex == 0xFFFFFFFFu) discard;   // texture not resident: the sprite isn't drawn either
    // Particle textures are UNORM (raw stored texel), multiplied by the sRGB colour as
    // the particle FS does.
    const vec4 c = texture(uTextures[nonuniformEXT(v_tex)], v_uv) * v_tint;
#if defined(TRANS_COLOR)
    if (c.a < 0.004) discard;
    outT = vec4(translucentTransmit(srgbToLinear(c.rgb), c.a), 1.0);
#elif defined(TRANS_DEPTH)
    if (c.a < 0.02) discard;
#else
    if (c.a < 0.5) discard;
#endif
}
