#version 460
#extension GL_EXT_fragment_shader_barycentric : require
#extension GL_GOOGLE_include_directive : require
// vkclean — RT TERMINATOR TRIANGLE PASS (#664; RenderVKRTTerminatorSource 1, needs
// VK_KHR_fragment_shader_barycentric). Runs right after the depth pre-pass over the SAME
// opaque / mask / rigged indirect draws with the SAME VS modules (invariant gl_Position),
// depth EQUAL, no depth write, early fragment tests: exactly one surviving fragment per
// pixel -- the surface the pre-pass kept, so no alpha test is needed here (a cut-out
// texel's fragment fails EQUAL against the depth behind it).
//
// It evaluates the Hanika shadow-terminator lift (clean_shadow_rt.glsl rtTerminatorTri)
// on the EXACT rasterized triangle: its three world positions and vertex normals come
// straight from the VS outputs (pervertexEXT) -- static from the mega-VBO through the
// instance cofactor, rigged from the pre-skinned + welded stream the raster shades with,
// terrain with its geomorph -- so the lift belongs to the very triangle the pixel shows.
// This replaces the camera-ray probe (a short ray through the receiver into the TLAS),
// whose closest hit near a crease's fold or a weld seam could be the OTHER surface and
// whose BLAS copy could differ from the raster (LOD, skinning) -- rounds 4-5 of #664.
//
// Output (R32G32_UINT, cleared 0 = no data):
//   x = facet normal, VISIBLE side (toward the eye), octahedral snorm16 x 2
//   y = bits 0-14  lift height above the facet, half float (sign bit dropped: >= 0)
//       bit  15    1 = valid
//       bits 16-31 smooth (interpolated vertex) normal, visible side, octahedral unorm8 x 2
// The lift is OUTWARD ONLY along the facet normal and size-capped (round 4); the
// light-independent escape check runs at the consumer (one short ray, in band only).
#include "clean_frame_info.glsl"   // ssr.cam_origin: the main eye

layout(early_fragment_tests) in;

layout(location = 2) pervertexEXT in vec3 v_world_normal_pv[3];
layout(location = 5) pervertexEXT in vec3 v_world_pos_pv[3];

layout(location = 0) out uvec2 outTri;

vec2 octWrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
vec2 octEncode(vec3 n)
{
    n /= (abs(n.x) + abs(n.y) + abs(n.z));
    return n.z >= 0.0 ? n.xy : octWrap(n.xy);
}

void main()
{
    const vec3 b  = gl_BaryCoordEXT;
    const vec3 p0 = v_world_pos_pv[0], p1 = v_world_pos_pv[1], p2 = v_world_pos_pv[2];
    const vec3 c  = cross(p1 - p0, p2 - p0);
    const float cl = length(c);
    if (!(cl > 1e-20)) { outTri = uvec2(0u); return; }
    const vec3 Ph = p0 * b.x + p1 * b.y + p2 * b.z;
    // The visible side (the raster flips the vertex normal with gl_FrontFacing).
    const float sd = dot(c, ssr.cam_origin.xyz - Ph) < 0.0 ? -1.0 : 1.0;
    const vec3 ng = c / cl * sd;
    const vec3 pv[3] = vec3[3](p0, p1, p2);
    vec3 lift = vec3(0.0), nsum = vec3(0.0);
    float dev = 0.0;
    for (int i = 0; i < 3; ++i)
    {
        vec3 n = v_world_normal_pv[i];
        const float nl = dot(n, n);
        n = (nl > 1e-20 && !any(isnan(n))) ? n * (inversesqrt(nl) * sd) : ng;
        // A vertex normal on the hidden side of its facet (inside-out / broken normals):
        // no tangent plane to trust -- the facet itself (no lift from this vertex).
        if (dot(n, ng) < 0.05) n = ng;
        dev = max(dev, length(cross(n, ng)));
        nsum += n * b[i];
        lift -= min(dot(Ph - pv[i], n), 0.0) * b[i] * n;
    }
    const float nsl = dot(nsum, nsum);
    const vec3 ns = nsl > 1e-20 ? nsum * inversesqrt(nsl) : ng;
    // Outward only, along the facet normal; capped at 0.75 x longest edge x deviation
    // (round 4: never binds on a smooth convex limb, bounds degenerate normal sets).
    float h = dot(lift, ng);
    if (any(isnan(lift)) || !(h > 0.0)) h = 0.0;
    const vec3 e01 = p1 - p0, e12 = p2 - p1, e20 = p0 - p2;
    const float emax = sqrt(max(max(dot(e01, e01), dot(e12, e12)), dot(e20, e20)));
    h = min(h, 0.75 * emax * dev);
    const uvec2 q = uvec2(round(clamp(octEncode(ns) * 0.5 + 0.5, 0.0, 1.0) * 255.0));
    outTri = uvec2(packSnorm2x16(octEncode(ng)),
                   (packHalf2x16(vec2(min(h, 60000.0), 0.0)) & 0x7FFFu) | 0x8000u | (q.x << 16) | (q.y << 24));
}
