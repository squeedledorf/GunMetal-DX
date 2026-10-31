#version 450
// LLRenderVK immediate-mode vertex shader, IN-WORLD LABEL variant (#487, #1126).
//
// Same vertex as ui.vert, for the pre-HUD text layer's anchored batches (name tags,
// chat bubbles, prim hover text), paired with ui_occl.frag. As LL / Firestorm depth-
// test the screen-facing quads of a tag, every PIXEL of the label is tested against
// the resolved scene depth at that pixel (ui_occl.frag): geometry in front hides the
// part of the tag it covers, never the whole tag for one stray depth sample at its
// anchor (#1126).
//
// The label's depth is its anchor's (the world point the text is attached to; the
// tag is screen-facing, so one depth serves every pixel), projected on the CPU with
// the world camera the depth was drawn with: the depth of the anchor pulled
// `bias + fade` metres toward the camera (`hide`) and the reciprocal of the span to
// the depth of the anchor pulled `bias` metres (`show`). Passed on flat; the fragment
// computes vis = clamp((hide - d) / (hide - show), 0, 1), 1 where the scene lies
// behind `show` and 0 where it lies in front of `hide`, for reversed-Z and
// conventional depth alike (the span's sign carries the convention).

layout(push_constant) uniform Push {
    mat4 mvp;                          // projection * modelview (as ui.vert)
    layout(offset = 64) vec4 anchor;   // xy: anchor depth-image UV (unused), z: hide depth, w: 1/(hide - show)
} pc;

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in vec2 aUV;

layout(location = 0) out vec4 vColor;
layout(location = 1) out vec2 vUV;
layout(location = 2) noperspective out vec2 vDepthUV;   // this vertex's depth-image UV
layout(location = 3) flat out vec2 vLabelDepth;         // x: hide depth, y: 1/(hide - show)

void main() {
    vColor = aColor;
    vUV = aUV;
    gl_Position = pc.mvp * vec4(aPos, 1.0);
    // Vulkan clip -> framebuffer UV (y down), as the anchor's UV on the CPU: the layer
    // and the scene depth both cover the whole frame.
    vDepthUV = (gl_Position.xy / gl_Position.w) * 0.5 + 0.5;
    vLabelDepth = pc.anchor.zw;
}
