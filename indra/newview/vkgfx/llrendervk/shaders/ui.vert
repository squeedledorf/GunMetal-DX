#version 450
// LLRenderVK immediate-mode vertex shader.
//
// Mirrors the classic gGL vertex: interleaved position (vec3), packed RGBA8
// color, and vec2 texcoord. The combined modelview*projection matrix arrives
// as a push constant (this is how LLRender's matrix stack is delivered per
// batch). Y-flip and Z[0,1] are already baked into the ortho matrix built by
// LLRenderVK::ortho(), so no per-shader clip fixup is needed.

layout(push_constant) uniform Push {
    mat4 mvp;   // projection * modelview  (built CPU-side, Vulkan clip convention)
} pc;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;   // already normalized 0..1 (unpacked CPU-side)
layout(location = 2) in vec2 aUV;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;

void main() {
    vColor = aColor;
    vUV = aUV;
    gl_Position = pc.mvp * vec4(aPos, 1.0);
}
