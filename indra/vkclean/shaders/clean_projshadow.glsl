// vkclean — per-projector shadow records (set 0 binding 19), shared by the lit FS and
// the ray-traced shadow mask.
struct ProjShadow {
    mat4 vp;        // CAMERA-RELATIVE world (P - ssr.cam_origin, #555) -> projector VK clip
                    // (rendered and sampled with the same world matrix)
    vec4 rect;      // atlas uv: offset .xy, size .zw
    vec4 params;    // .x world texel per unit distance, .y RT: near distance from the apex,
                    // .z technique (2 = ray traced), .w valid
    vec4 texel;     // map: .xy atlas texel (uv), .z normal-offset scale, .w light-offset scale
                    // RT:  .x light radius (m, soft shadows), .y normal offset (m),
                    //      .z rays per pixel (soft only)
};
layout(std430, set = 0, binding = 19) readonly buffer ProjShadows { ProjShadow pshadow[]; };
