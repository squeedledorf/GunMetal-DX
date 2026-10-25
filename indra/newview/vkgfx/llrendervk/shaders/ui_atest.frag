#version 450
// LLRenderVK immediate-mode fragment shader, ALPHA-TESTED variant (#83).
//
// Used only inside offscreen render targets (LLViewerDynamicTexture under Vulkan),
// where LL's GL code relies on gAlphaMaskProgram's minimum_alpha discard -- the
// avatar compositor most of all: after a layer's morph masks are rendered it blends
// with (DST_ALPHA, ONE_MINUS_DST_ALPHA), so a texel the alpha test would have
// discarded still writes its colour. Same output as ui.frag otherwise.

layout(set = 0, binding = 0) uniform sampler2D uTex;

layout(push_constant) uniform Push {
    layout(offset = 80) float minAlpha;   // after the vertex stage's mat4 mvp + label anchor (#487)
} pc;

layout(location = 0) in vec4 vColor;
layout(location = 1) in vec2 vUV;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 c = texture(uTex, vUV) * vColor;
    if (c.a < pc.minAlpha) discard;
    outColor = c;
}
