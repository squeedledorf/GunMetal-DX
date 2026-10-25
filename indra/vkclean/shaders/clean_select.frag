#version 450
// vkclean — edit-mode selection outline (LL renderSilhouettes' per-face wireframe).
// Drawn with polygonMode LINE over the selected instances; the colour (LL's
// silhouette parent/child colour, alpha dimmed for occluded edges) rides in the
// draw view's last vec4 (water_fog slot, unused by this pass).
#extension GL_GOOGLE_include_directive : require
layout(location = 0) out vec4 outColor;

#include "clean_draw_view.glsl"   // #238: the view block (pc.u_*) + 16 B push

void main()
{
    outColor = pc.u_water_fog;   // the selection colour
}
