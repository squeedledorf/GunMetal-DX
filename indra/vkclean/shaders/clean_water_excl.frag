#version 450
#extension GL_EXT_nonuniform_qualifier : require
#extension GL_GOOGLE_include_directive : require
// vkclean — WATER EXCLUSION MASK (#27; LL LLDrawPoolWaterExclusion).
//
// An R8 target at render resolution, cleared to 1. Drawn over the exclusion list (F2:
// the alpha sort scatters every exclusion instance there; RenderVKWaterLists off, the
// post-water alpha list, where they are forced and the FS discards the rest), with
// the pass-1 scene depth as a read-only depth test: invisiprims (instance flag bit 16)
// write 0 where they cover the water, and clean_water.frag discards the water surface
// wherever the mask is below 1 (boat hulls stay dry inside).
//
// LL draws the water planes white, then the invisiprims black, depth-tested against
// those planes: an invisiprim only masks where it lies between the camera and the
// water. With one water plane that is exactly "on the camera's side of the water
// height", tested per fragment here (>= / <=: LL's LEQUAL depth test keeps a face
// lying on the water line). LL keeps GL_CULL_FACE on for PASS_INVISIBLE: single-sided
// back faces drop out (the pipeline itself culls nothing, like the alpha pipelines).
#include "clean_draw_common.glsl"

void main()
{
    if ((v_flags & 65536u) == 0u) discard;                              // exclusion surfaces only
    if (!gl_FrontFacing && (v_flags & 256u) == 0u) discard;             // single-sided back face
    const float wh = waterHeight();
    const bool cam_side = camUnderwater() ? (v_world_pos.z <= wh) : (v_world_pos.z >= wh);
    if (!cam_side) discard;
    outColor = vec4(0.0);
}
