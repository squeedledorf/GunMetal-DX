#version 450
// vkclean — fullscreen triangle for the reflection-probe capture sky.
layout(location = 0) out vec2 v_uv;

void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 1.0, 1.0);   // depth 1: behind everything
}
