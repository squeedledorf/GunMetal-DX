#version 450
// vkclean -- VR VISIBILITY MASK (#1075, XR_KHR_visibility_mask; LLCleanRendererVK::recordVisMask).
//
// Lays each eye's hidden-area triangles -- the pixels the headset's lens never shows -- into
// that eye's depth layer at the reversed-Z nearest value (1.0) as the first draw of the stereo
// pass 1 (depth ALWAYS + write, no colour, no fragment shader). Every later depth-tested draw of
// the frame (the pre-pass LESS, the lit EQUAL, the sky EQUAL-far, water, alpha, particles) is
// then rejected there by early-Z instead of shaded. One multiview draw: CLEAN_VIEW (gl_ViewIndex)
// picks the eye's list.
//
// Vertices: x y pairs in the eye's view space on the z = -1 plane (as the runtime hands them
// out), pulled from a buffer by device address. Eye v's list starts at vertex first[v] and has
// count[v] vertices (whole triangles); the draw has max(count) vertices and an eye's surplus
// collapses onto one point outside the depth range (clipped). Clip position: the eye's own
// projection P at z = -1 (w = 1): x' = P00 x - P20, y' = P11 y - P21 (proj = P00, P20, P11, P21
// of the eye's column-major projection: the off-axis terms and the Vulkan Y flip included);
// depth = w (NDC 1.0, the reversed-Z near plane).
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

#include "clean_view.glsl"

layout(buffer_reference, std430, buffer_reference_align = 8) readonly buffer MaskVerts { vec2 v[]; };

layout(push_constant) uniform VisMaskPush {
    vec4  proj0;   // eye 0: P00, P20, P11, P21
    vec4  proj1;   // eye 1
    uvec2 verts;   // MaskVerts device address
    uvec2 first;   // per eye: first vertex
    uvec2 count;   // per eye: vertex count (a multiple of 3)
} pc;

void main()
{
    const uint view = CLEAN_VIEW;
    const vec4 p = view == 0u ? pc.proj0 : pc.proj1;
    const uint n = view == 0u ? pc.count.x : pc.count.y;
    const uint i = uint(gl_VertexIndex);
    if (i >= n)
    {
        gl_Position = vec4(0.0, 0.0, -1.0, 1.0);   // degenerate + outside [0, 1] depth: clipped
        return;
    }
    const vec2 t = MaskVerts(pc.verts).v[(view == 0u ? pc.first.x : pc.first.y) + i];
    gl_Position = vec4(p.x * t.x - p.y, p.z * t.y - p.w, 1.0, 1.0);
}
