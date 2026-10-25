#version 450

// Stage 3: a large quad (two triangles) covering most of the frame, textured.
// Emitted via gl_VertexIndex (6 verts). UV top-left origin to match image data.
layout(location = 0) out vec2 vUV;

void main() {
    vec2 pos[6] = vec2[](
        vec2(-0.9, -0.9), vec2( 0.9, -0.9), vec2( 0.9,  0.9),
        vec2(-0.9, -0.9), vec2( 0.9,  0.9), vec2(-0.9,  0.9)
    );
    // UV: (0,0) at top-left of the quad. Since Vulkan clip y=-1 is top, the
    // top-left vertex is (-0.9,-0.9) -> uv (0,0).
    vec2 uv[6] = vec2[](
        vec2(0.0, 0.0), vec2(1.0, 0.0), vec2(1.0, 1.0),
        vec2(0.0, 0.0), vec2(1.0, 1.0), vec2(0.0, 1.0)
    );
    gl_Position = vec4(pos[gl_VertexIndex], 0.0, 1.0);
    vUV = uv[gl_VertexIndex];
}
