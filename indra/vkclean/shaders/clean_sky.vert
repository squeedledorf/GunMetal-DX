#version 450
// vkclean — sky full-screen pass (vertex).
//
// Emits a single oversized triangle covering the whole viewport; no vertex
// buffer. Passes NDC to the fragment shader, which un-projects it to a world
// view direction. The sky is drawn before the world with depth writes off, so
// it acts as a pure background fill.

layout(location = 0) out vec2 v_ndc;

void main()
{
    // Fullscreen-triangle trick: 3 verts covering NDC [-1,3].
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vec2 pos = uv * 2.0 - 1.0;               // (-1,-1),(3,-1),(-1,3)
    gl_Position = vec4(pos, 0.0, 1.0);       // depth 0; depth test off anyway
    v_ndc = pos;
}
