#version 450
// <GMDX> vkclean -- SELECTION OUTLINE MASK (GMDXSelectionStyle 1, vkgfx/gmdxseloutlinevk.cpp).
// The selected faces (the selection outline's own draw lists, static + rigged, world or HUD)
// drawn FILLED into an R16_UINT mask at display size, against the scene depth (read-only):
//   bits 0..1  kind + 1 (1 root, 2 child, 3 hover preview)
//   bit  2     occluded (the hidden pass: depth GREATER, drawn first; the visible pass
//              overwrites it where the face is in front)
//   bits 3..15 a hash of the instance id (one instance per face: a face edge is an id change)
// and marks its 16 x 16 pixel tile, so the edge pass only runs where there is something.
// Kind and pass ride in the draw view's last vec4 (water_fog), as clean_select.frag's colour:
//   .x mask value bits 0..2   .y tiles per row   .z tile count
#extension GL_GOOGLE_include_directive : require
layout(location = 12) flat in uint v_inst;   // clean_draw*.vert CLEAN_VS_PICK: gl_InstanceIndex
layout(location = 0) out uint outMask;

#include "clean_draw_view.glsl"

layout(std430, set = 2, binding = 0) writeonly buffer SelTiles { uint sel_tiles[]; };

void main()
{
    const uint face = (v_inst * 2654435761u) >> 19;   // 13 bits
    outMask = (uint(pc.u_water_fog.x + 0.5) & 7u) | (face << 3);
    const uvec2 t = uvec2(gl_FragCoord.xy) >> 4u;
    const uint idx = t.x + t.y * uint(pc.u_water_fog.y + 0.5);
    if (idx < uint(pc.u_water_fog.z + 0.5)) sel_tiles[idx] = 1u;
}
