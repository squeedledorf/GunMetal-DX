#version 450
#extension GL_EXT_nonuniform_qualifier : require
// Stage 5 Part A — GPU-driven instanced cube vertex shader.
//
// Vertex positions/normals come from the shared cube VBO. The per-draw
// gl_InstanceIndex is set by firstInstance in the indirect command (the compute
// shader wrote the surviving instance id there), so each draw pulls its own
// model matrix and texture index from the instance SSBO.

layout(location = 0) in vec3 inPos;
layout(location = 1) in vec3 inNormal;

struct Instance {
    mat4 model;
    vec4 sphere;
    uvec4 misc;   // x = texIndex
};
layout(std430, set = 0, binding = 0) readonly buffer Instances {
    Instance instances[];
};

layout(push_constant) uniform Push {
    mat4 viewProj;
} pc;

layout(location = 0) out vec3 vNormalWS;
layout(location = 1) out vec2 vUV;
layout(location = 2) flat out uint vTexIndex;

// Simple planar UV from object-space position so the texture is visible.
void main() {
    Instance inst = instances[gl_InstanceIndex];
    vec4 world = inst.model * vec4(inPos, 1.0);
    gl_Position = pc.viewProj * world;
    vNormalWS = mat3(inst.model) * inNormal;

    // Pick two dominant axes of the face normal for a stable planar UV.
    vec3 an = abs(inNormal);
    if (an.x > an.y && an.x > an.z)      vUV = inPos.zy + 0.5;
    else if (an.y > an.z)                vUV = inPos.xz + 0.5;
    else                                 vUV = inPos.xy + 0.5;

    vTexIndex = inst.misc.x;
}
