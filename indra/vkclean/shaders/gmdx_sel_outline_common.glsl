// <GMDX> vkclean -- SELECTION OUTLINE edge pass push block (vkgfx/gmdxseloutlinevk.cpp
// SelOutlinePush, 80 B).
#ifndef GMDX_SEL_OUTLINE_COMMON_GLSL
#define GMDX_SEL_OUTLINE_COMMON_GLSL
layout(push_constant) uniform SelOutlinePush {
    vec4 colors[3];   // kind 1 root, 2 child, 3 hover preview: rgb, a opacity
    vec4 dims;        // tiles per row, tile rows, display width, height (px)
    vec4 params;      // x line width (px), y occluded edges shown (1 / 0), z their opacity scale
} p;
#endif
