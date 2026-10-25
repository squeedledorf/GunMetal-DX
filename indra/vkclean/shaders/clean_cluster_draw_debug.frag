#version 450
// vkclean -- CLUSTER LOD debug views into the G-buffer (#745, OWNER: 1b; RenderVKClusterDebug
// 1 colour by cluster, 2 by DAG level, 3 red = parent drawn because the finer page is
// missing / green = own level). The colour arrives as the flat tint (clean_cluster_draw_common
// clusterVertex); written as a plain legacy (lit) surface so the shape stays readable. Same
// back-face rule as clean_draw.frag (single-sided back faces discarded). Packing =
// clean_gbuffer.glsl gbufferPack (SURF_LEGACY, no gloss / glow / emissive).
layout(location = 2)      in vec3 v_world_normal;
layout(location = 3) flat in vec4 v_tint;
layout(location = 4) flat in uint v_flags;

layout(location = 0) out vec4 outG0;
layout(location = 1) out vec4 outG1;
layout(location = 2) out vec4 outG2;
layout(location = 3) out vec4 outG3;

vec2 dbgOctWrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec2 dbgOctEncode(vec3 n)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-20);
    return n.z >= 0.0 ? n.xy : dbgOctWrap(n.xy);
}

void main()
{
    if (!gl_FrontFacing && (v_flags & (256u | 8u)) == 0u) discard;
    vec3 N = normalize(v_world_normal);
    if (!gl_FrontFacing) N = -N;
    const uint SURF_LEGACY = 2u;
    outG0 = vec4(clamp(v_tint.rgb, vec3(0.0), vec3(1.0)), 0.0);
    outG1 = vec4(dbgOctEncode(N), dbgOctEncode(N));
    outG2 = vec4(0.0, 0.0, 0.0, float(SURF_LEGACY | ((v_flags & 16384u) != 0u ? 4u : 0u)) * (1.0 / 255.0));
    outG3 = vec4(0.0);
}
