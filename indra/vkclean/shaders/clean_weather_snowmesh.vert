#version 450
#extension GL_GOOGLE_include_directive : require
// vkclean — SNOW SURFACE MESH draw (#783 M3; clean_weather_snowmesh.comp builds it,
// vkgfx/llcleanweathervk.cpp recordMeshDraw). The world draw layout (draw sets 0 / 1 + the
// 16 B view push: clean_draw_view.glsl), the mesh's own vertex / index buffers bound as such:
// one indexed indirect draw per pass (the inner grid's and the outer ring's triangles alike).
// -DWX_MESH_DEPTH: the shadow cascades' position-only variant (no fragment stage: both layers
// cast where they overlap in the hand-over band -- the same surface, no double darkening).
// -DWX_MESH_FORWARD: the FORWARD lit pass 1's variant (frames that do not go deferred: a
// supersampled snapshot with RT shadows, RenderVKSnapshotDeferred off, debug views): the lit
// FS's interface (clean_draw_common.glsl) for clean_draw.frag -DCLEAN_WEATHER_SNOWMESH --
// the hand-over band rides in v_uv.
#include "clean_draw_view.glsl"

// The depth pre-pass draw (clean_weather_snowmesh.frag -DWX_MESH_PRE) and the later G-buffer /
// forward draw (LEQUAL) must land on the same depth -- across pipelines and, in a forward frame,
// across this VS and its WX_MESH_FORWARD twin (the world VS's rule: clean_draw.vert).
invariant gl_Position;

layout(location = 0) in vec3 a_pos;   // world (agent) position of the snow surface
layout(location = 1) in uint a_nrm;    // octahedral 12-bit x | y << 12 | the band byte << 24

#if defined(WX_MESH_FORWARD)
layout(location = 0) flat out uint v_material_id;
layout(location = 1)      out vec2 v_band;          // (the lit FS's v_uv)
layout(location = 2)      out vec3 v_nrm;           // (v_world_normal)
layout(location = 3) flat out vec4 v_tint;
layout(location = 4) flat out uint v_flags;
layout(location = 5)      out vec3 v_wpos;          // (v_world_pos)
layout(location = 7)      out vec4 v_world_tangent;
layout(location = 8)      out vec2 v_uv_raw;
layout(location = 9) flat out uint v_terrain_rec;
#elif !defined(WX_MESH_DEPTH)
layout(location = 0) out vec3 v_nrm;
layout(location = 1) out vec3 v_wpos;
layout(location = 2) out vec2 v_band;  // x the hand-over t (0 inner .. 1 ring), y 1 = the ring's
layout(location = 3) out float v_dbg;  // #783 the depth debug's code (bits 0-1 the stride: 1 stride 2, 2 stride 4; 4 an apron vertex)
#endif
#ifndef WX_MESH_DEPTH

vec3 octDecode(uint w)
{
    const vec2 e = max(vec2(float(int(w << 20) >> 20), float(int(w << 8) >> 20)) / 2047.0, vec2(-1.0));
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}
#endif

#ifdef WX_MESH_DEPTH
// #783 HAND-OVER IN THE SUN CASCADES: the G-buffer interleaves the two layers by a screen-space
// dither, which a shadow map cannot do -- both cast in full across the band. The inner layer there
// is still its own fine surface near the band's start (it morphs onto the ring's only by its
// end), so the coarse ring stood above it in places (ridges, kerbs, berms, a bridge deck's edge)
// and shadowed it: a thin dark line along the fine grid's hand-over. Here the RING sinks under
// the inner layer while t < 1 (the inner layer is the band's caster; at t = 1 the two surfaces
// are one and the ring casts alone beyond): kRingSink x (1 - t) metres, linear in t, so the
// ring's triangles stay continuous.
const float kRingSink = 1.0;
#endif

void main()
{
    // (camera-relative, #555: the view's origin is subtracted before the projection)
#ifdef WX_MESH_DEPTH
    vec3 p = a_pos;
    {
        const uint b = a_nrm >> 24u;
        if ((b & 128u) != 0u) p.z -= kRingSink * (1.0 - float(b & 127u) / 127.0);
    }
    gl_Position = pc.u_view_proj * vec4(p - pc.u_origin.xyz, 1.0);
#else
    gl_Position = pc.u_view_proj * vec4(a_pos - pc.u_origin.xyz, 1.0);
#endif
#ifndef WX_MESH_DEPTH
    v_nrm = octDecode(a_nrm);
    v_wpos = a_pos;
    const uint b = a_nrm >> 24u;
    v_band = vec2(float(b & 127u) / 127.0, float(b >> 7u));
#if !defined(WX_MESH_FORWARD)
    v_dbg = float(a_nrm & 15u);
#endif
#endif
#ifdef WX_MESH_FORWARD
    v_material_id = 0u;
    v_tint = vec4(1.0);
    v_flags = 0u;                     // (an opaque world surface)
    v_world_tangent = vec4(0.0);      // (no tangent: the FS builds its normal itself)
    v_uv_raw = vec2(0.0);
    v_terrain_rec = 0xFFFFFFFFu;
#endif
}
