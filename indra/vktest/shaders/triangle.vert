#version 450

// Stage 2: colored triangle. No vertex buffer — positions/colors are baked in
// and selected by gl_VertexIndex. Vulkan clip space: Y is down, so we author
// the triangle so it appears point-up in the final (top-to-bottom) PNG.
layout(location = 0) out vec3 vColor;

void main() {
    // Clip-space positions. In Vulkan, y = -1 is the TOP of the framebuffer.
    // Apex at top (y negative), base at bottom (y positive).
    vec2 positions[3] = vec2[](
        vec2( 0.0, -0.7),   // apex (top)
        vec2(-0.7,  0.7),   // bottom-left
        vec2( 0.7,  0.7)    // bottom-right
    );
    vec3 colors[3] = vec3[](
        vec3(1.0, 0.0, 0.0),  // red apex
        vec3(0.0, 1.0, 0.0),  // green bottom-left
        vec3(0.0, 0.0, 1.0)   // blue bottom-right
    );
    gl_Position = vec4(positions[gl_VertexIndex], 0.0, 1.0);
    vColor = colors[gl_VertexIndex];
}
