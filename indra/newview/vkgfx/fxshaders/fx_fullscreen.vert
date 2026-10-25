#version 450
// Slipstream FX host (#544): fullscreen triangle for the host's own draws (final copy,
// per-effect scope composite, HDR in / out). Positive viewport; v_uv (0,0) = top-left.
layout(location = 0) out vec2 v_uv;

void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    v_uv = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
