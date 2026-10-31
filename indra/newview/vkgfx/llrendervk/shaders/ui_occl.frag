#version 450
// LLRenderVK immediate-mode fragment shader, IN-WORLD LABEL variant (#1126).
//
// ui.frag, with the pixel faded / hidden by the scene depth at that pixel against
// the label's depth (ui_occl.vert): the per-pixel test LL / Firestorm get from the
// GL depth test on a tag's screen-facing quads, with the label's camera-ward bias
// and fade band kept.

layout(set = 0, binding = 0) uniform sampler2D uTex;
layout(set = 1, binding = 0) uniform sampler2D uSceneDepth;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;
layout(location = 2) noperspective in vec2 vDepthUV;
layout(location = 3) flat in vec2 vLabelDepth;   // x: hide depth, y: 1/(hide - show)

layout(location = 0) out vec4 outColor;

void main() {
    ivec2 size = textureSize(uSceneDepth, 0);
    ivec2 p = clamp(ivec2(vDepthUV * vec2(size)), ivec2(0), size - 1);
    float d = texelFetch(uSceneDepth, p, 0).r;
    float vis = clamp((vLabelDepth.x - d) * vLabelDepth.y, 0.0, 1.0);
    if (vis <= 0.0) discard;
    vec4 c = texture(uTex, vUV) * vColor;
    outColor = vec4(c.rgb, c.a * vis);
}
