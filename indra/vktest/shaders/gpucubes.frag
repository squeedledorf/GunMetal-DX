#version 450
#extension GL_EXT_nonuniform_qualifier : require
// Stage 5 Part A — bindless textured + lit fragment shader.
//
// The per-instance texture index (flat, from the vertex shader) selects one
// element of a runtime-sized combined-image-sampler array via a nonuniform
// index — the descriptor-indexing path a GPU-driven bindless renderer needs.

layout(location = 0) in vec3 vNormalWS;
layout(location = 1) in vec2 vUV;
layout(location = 2) flat in uint vTexIndex;
layout(location = 0) out vec4 outColor;

layout(set = 1, binding = 0) uniform sampler2D uTextures[];

void main() {
    vec3 N = normalize(vNormalWS);
    vec3 L = normalize(vec3(0.4, 0.8, 0.5));
    float ndotl = max(dot(N, L), 0.0);
    float ambient = 0.25;

    vec4 tex = texture(uTextures[nonuniformEXT(vTexIndex)], vUV);
    vec3 color = tex.rgb * (ambient + ndotl);
    outColor = vec4(color, 1.0);
}
