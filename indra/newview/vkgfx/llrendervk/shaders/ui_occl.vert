#version 450
// LLRenderVK immediate-mode vertex shader, IN-WORLD LABEL variant (#487).
//
// Same vertex as ui.vert, for the pre-HUD text layer's anchored batches (name tags,
// chat bubbles, prim hover text). The whole label is tested ONCE, at its anchor (the
// world point the text is attached to), against the resolved scene depth: every
// vertex of the label computes the same visibility, so a tag is shown, faded or
// hidden as a whole and never half-clipped by the geometry its glyphs overlap.
//
// The anchor arrives pre-projected (CPU, with the world camera the depth was drawn
// with): its depth-image UV, the depth of the anchor pulled `bias + fade` metres
// toward the camera (`hide`) and the reciprocal of the span to the depth of the
// anchor pulled `bias` metres (`show`). vis = clamp((hide - d) / (hide - show), 0, 1)
// is 1 where the scene lies behind `show` and 0 where it lies in front of `hide`, for
// reversed-Z and conventional depth alike (the span's sign carries the convention).
// Five taps (centre + a small cross) average out the TAA jitter of the native-scale
// depth and fade a label smoothly as its anchor slides behind an edge.

layout(set = 1, binding = 0) uniform sampler2D uSceneDepth;

layout(push_constant) uniform Push {
    mat4 mvp;                          // projection * modelview (as ui.vert)
    layout(offset = 64) vec4 anchor;   // xy: depth-image UV, z: hide depth, w: 1/(hide - show)
} pc;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;

float tapVis(ivec2 p, ivec2 size) {
    float d = texelFetch(uSceneDepth, clamp(p, ivec2(0), size - 1), 0).r;
    return clamp((pc.anchor.z - d) * pc.anchor.w, 0.0, 1.0);
}

void main() {
    ivec2 size = textureSize(uSceneDepth, 0);
    ivec2 c = ivec2(pc.anchor.xy * vec2(size));
    const int r = 2;   // depth-image pixels
    float vis = tapVis(c, size);
    vis += tapVis(c + ivec2( r, 0), size);
    vis += tapVis(c + ivec2(-r, 0), size);
    vis += tapVis(c + ivec2(0,  r), size);
    vis += tapVis(c + ivec2(0, -r), size);
    vis *= 0.2;

    vColor = vec4(aColor.rgb, aColor.a * vis);
    vUV = aUV;
    // Fully hidden: drop the primitive (outside the clip volume) -- no fragments.
    gl_Position = vis > 0.0 ? pc.mvp * vec4(aPos, 1.0) : vec4(2.0, 2.0, 2.0, 1.0);
}
