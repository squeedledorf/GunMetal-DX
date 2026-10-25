#version 450
// vkclean — occlusion-query proxy box (vertex).
//
// Draws a unit cube [-1,1]^3 (36 verts from gl_VertexIndex, no vertex buffer)
// transformed by a single push-constant matrix: world->clip * box model (the box
// model already carries position, rotation, half-extents and a small world-space
// inflation). Used inside pass 1 with depth test LEQUAL, no depth/colour writes,
// wrapped in a VK_QUERY_TYPE_OCCLUSION query (planar mirror visibility gate).

layout(push_constant) uniform OccPush
{
    mat4 mvp;
} pc;

const vec3 kCorners[8] = vec3[8](
    vec3(-1.0, -1.0, -1.0), vec3( 1.0, -1.0, -1.0),
    vec3( 1.0,  1.0, -1.0), vec3(-1.0,  1.0, -1.0),
    vec3(-1.0, -1.0,  1.0), vec3( 1.0, -1.0,  1.0),
    vec3( 1.0,  1.0,  1.0), vec3(-1.0,  1.0,  1.0));

// 12 triangles (winding irrelevant: no culling).
const int kIdx[36] = int[36](
    0, 1, 2,  0, 2, 3,   // -Z
    4, 6, 5,  4, 7, 6,   // +Z
    0, 4, 5,  0, 5, 1,   // -Y
    3, 2, 6,  3, 6, 7,   // +Y
    0, 3, 7,  0, 7, 4,   // -X
    1, 5, 6,  1, 6, 2);  // +X

void main()
{
    gl_Position = pc.mvp * vec4(kCorners[kIdx[gl_VertexIndex]], 1.0);
}
