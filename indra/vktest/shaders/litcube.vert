#version 450

// Stage 4: lit cube. Positions + normals from vertex buffers, MVP via push
// constant. GLM builds the matrix with GLM_FORCE_DEPTH_ZERO_TO_ONE and a
// Y-flip already folded into the projection, so gl_Position is Vulkan-correct.
layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

layout(push_constant) uniform PushConstants {
    mat4 mvp;
    mat4 model;   // for transforming the normal into world space
} pc;

layout(location = 0) out vec3 vNormalWS;

void main() {
    gl_Position = pc.mvp * vec4(inPos, 1.0);
    // Model is a pure rotation+translation (no non-uniform scale), so the
    // upper-left 3x3 is fine for normals.
    vNormalWS = mat3(pc.model) * inNormal;
}
