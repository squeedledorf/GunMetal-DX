// vkclean — depth of field (RenderDepthOfField), port of LL's renderDoF chain:
// cofF (colour + circle of confusion) -> postDeferredF (gather at CameraDoFResScale)
// -> dofCombineF. Runs on the LINEAR HDR scene ahead of bloom and the tonemapper
// (upstream Alchemy's order: gathering display-space values gathers already
// compressed highlights). Shared push block for the three passes.
layout(location = 0) in vec2 v_uv;
layout(location = 0) out vec4 outColor;

layout(push_constant) uniform DofPush {
    mat4 inv_proj;   // VK clip -> view (current frame)
    vec4 lens;       // focal_distance (view z, m), blur_constant, tan_pixel_angle, magnification
    vec4 params;     // max_cof (px), res_scale, near blur (0/1), -
    vec4 screen;     // full-res width, height, 1/width, 1/height
} pc;
