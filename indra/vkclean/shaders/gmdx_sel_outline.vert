#version 450
// <GMDX> vkclean -- SELECTION OUTLINE edge pass, vertex stage (vkgfx/gmdxseloutlinevk.cpp).
// One instance per 16 x 16 pixel tile of the display; a tile becomes a quad only when it or a
// neighbour holds selected pixels (gmdx_sel_mask.frag marked them), so the edge test never
// runs over the whole screen. No vertex input: 6 vertices per instance.
#extension GL_GOOGLE_include_directive : require
#include "gmdx_sel_outline_common.glsl"

layout(std430, set = 0, binding = 1) readonly buffer SelTiles { uint sel_tiles[]; };

void main()
{
    const int tiles_x = int(p.dims.x), tiles_y = int(p.dims.y);
    const int tx = gl_InstanceIndex % tiles_x, ty = gl_InstanceIndex / tiles_x;
    bool any = false;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
        {
            const int x = tx + dx, y = ty + dy;
            if (x >= 0 && y >= 0 && x < tiles_x && y < tiles_y && sel_tiles[x + y * tiles_x] != 0u)
                any = true;
        }
    if (!any)
    {
        gl_Position = vec4(2.0, 2.0, 0.0, 1.0);   // every corner the same: no area, culled
        return;
    }
    // Two triangles: corners (0,0) (1,0) (0,1) / (1,0) (1,1) (0,1).
    const int v = gl_VertexIndex;
    const ivec2 c = ivec2((v == 1 || v == 3 || v == 4) ? 1 : 0, (v == 2 || v == 4 || v == 5) ? 1 : 0);
    const vec2 px = min(vec2((ivec2(tx, ty) + c) * 16), p.dims.zw);
    gl_Position = vec4(px / p.dims.zw * 2.0 - 1.0, 0.0, 1.0);
}
