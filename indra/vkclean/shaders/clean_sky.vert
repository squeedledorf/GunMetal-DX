#version 450
#extension GL_GOOGLE_include_directive : require
// vkclean — sky full-screen pass (vertex).
//
// Emits a single oversized triangle covering the whole viewport; no vertex
// buffer. Passes NDC to the fragment shader, which un-projects it to a world
// view direction. The sky is drawn before the world with depth writes off, so
// it acts as a pure background fill.
//
// #1075 LAYERED STEREO (multiview, kViewCount > 1): the push's inverse is the head's, so each
// view un-projects its own corner here instead -- the inverse of its (P - cam_origin) -> clip
// matrix (ssr.views[v].taa_cur_vp), shifted to its eye (the eye's offset from cam_origin, from
// its world -> view matrix), as a homogeneous point at NDC depth 0.5. Linear in screen space, so
// the fragment shader's v_eye_p.xyz / v_eye_p.w is that pixel's direction from the eye. Three
// vertices: the mat4 inverse is free. One view (every desktop pipeline): folded away, v_eye_p 0.
#include "clean_frame_info.glsl"

layout(location = 0) out vec2 v_ndc;
layout(location = 1) noperspective out vec4 v_eye_p;

void main()
{
    // Fullscreen-triangle trick: 3 verts covering NDC [-1,3].
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vec2 pos = uv * 2.0 - 1.0;               // (-1,-1),(3,-1),(-1,3)
    gl_Position = vec4(pos, 0.0, 1.0);       // depth 0; depth test off anyway
    v_ndc = pos;
    v_eye_p = vec4(0.0);
    if (kViewCount > 1)
    {
        const uint v = CLEAN_VIEW;
        vec4 p = inverse(ssr.views[v].taa_cur_vp) * vec4(pos, 0.5, 1.0);
        const mat4 V = ssr.views[v].cur_view;
        const vec3 eye = -(transpose(mat3(V)) * V[3].xyz);   // the eye, world
        p.xyz -= (eye - ssr.cam_origin.xyz) * p.w;           // relative to the eye
        v_eye_p = p;
    }
}
