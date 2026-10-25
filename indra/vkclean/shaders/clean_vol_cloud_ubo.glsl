// vkclean -- VOLUMETRIC CLOUDS (#719): the module's per-recording parameter block
// (LLCleanVolCloudsVK::Params, vkCmdUpdateBuffer before the first pass that reads it).
// Byte layout = LLCleanVolCloudsVK::Params (std140: vec4 / mat4 only). Set VC_SET, binding 0.
#ifndef CLEAN_VOL_CLOUD_UBO_GLSL
#define CLEAN_VOL_CLOUD_UBO_GLSL
#ifndef VC_SET
#define VC_SET 1
#endif
layout(std140, set = VC_SET, binding = 0) uniform VolCloudParams {
    vec4 shape0;   // x weather tile (m), y 1 / tile, z 1 / base noise period (m), w 1 / detail period (m)
    vec4 shape1;   // xy wind offset of the 3D noises (m), z extinction per m at density 1, w detail erosion
    vec4 shape2;   // x ambient scale, y max distance (m), z multi-scatter strength, w powder strength
    vec4 phase;    // x forward g, y backward g, z backward lobe weight, w (unused)
    vec4 march;    // x max steps (main view), y light steps, z frame index, w 1 = offscreen (snapshot)
    vec4 cam;      // xyz camera world position, w time (s)
    vec4 shadow;   // shadow map: xy centre (m), z extent (m), w steps
    vec4 pano;     // x first row, y rows this dispatch, z steps, w history blend (0 = overwrite)
    // Main view (march / temporal / composite):
    mat4 inv_vp;   // UNJITTERED VK clip -> CAMERA-RELATIVE world (this frame)
    mat4 reproj;   // camera-relative world (this frame) -> last frame's unjittered clip
    vec4 dims;     // render w, h, march w, h
    vec4 pat;      // x pattern (0 all, 1 checkerboard, 2 one per 2x2), y frame phase, z steps, w light steps
    vec4 temporal; // x max history (frames), y 1 = history reset, z clamp widening, w (unused)
    vec4 flash;    // #783 M2 lightning: xyz unit direction to the strike (world), w its radiance (0 = none)
} vc;
#endif
