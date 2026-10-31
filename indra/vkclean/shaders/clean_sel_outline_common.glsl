// vkclean -- (ported from GunMetal-DX, squeedledorf, LGPL 2.1, v0.3.0 cf5f1f4) SELECTION OUTLINE edge pass push block (vkgfx/llcleanseloutlinevk.cpp
// SelOutlinePush, 80 B).
#ifndef CLEAN_SEL_OUTLINE_COMMON_GLSL
#define CLEAN_SEL_OUTLINE_COMMON_GLSL
layout(push_constant) uniform SelOutlinePush {
    vec4 colors[3];   // kind 1 root, 2 child, 3 hover preview: rgb, a opacity
    vec4 dims;        // tiles per row, tile rows, display width, height (px)
    vec4 params;      // x line width (px), y occluded edges shown (1 / 0), z their opacity scale
} p;
#endif
