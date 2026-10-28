// vkclean -- AMBIENT OCCLUSION shared helpers (#86): the receiver reconstruction, the
// normal sources and the half-resolution grid of clean_ao.comp (SSAO / GTAO / blurs /
// temporal accumulator) and clean_rtao.comp (ray-traced AO, RenderVKAOMethod 2), so every
// AO producer evaluates the SAME receiver (representative pixel, depth, normals) and writes
// the same upsample meta. The includer declares, before including this file:
//   * the push block `pc` with mat4 inv_proj (VK clip -> view) and vec4 screen (full render
//     width, height, 1 / width, 1 / height) as its first members;
//   * uDepth (sampler2D, the pre-pass depth, NEAREST), uNormal (sampler2D, the pre-pass
//     normal target or G1) and the spec constant kAoGbufNormals (constant_id 0);
//   * the frame-info block (clean_frame_info.glsl: ssr.views[CLEAN_VIEW].cur_view).
#ifndef CLEAN_AO_COMMON_GLSL
#define CLEAN_AO_COMMON_GLSL
float depthAt(vec2 tc) { return textureLod(uDepth, tc, 0.0).r; }

vec3 viewPos(vec2 tc, float d)
{
    vec4 p = pc.inv_proj * vec4(tc * 2.0 - 1.0, d, 1.0);   // VK NDC z = depth
    return p.xyz / p.w;
}
vec3 viewPos(vec2 tc) { return viewPos(tc, depthAt(tc)); }

// Pre-pass normals (clean_prepass_normal.frag octEncode): .xy the shading normal
// (normal-mapped, what the lit FS shades with), .zw (#648 round 2) the SMOOTH
// interpolated vertex normal, unperturbed. The depth-MIP path orients its hemisphere /
// tangent plane / blur plane test on .zw: smooth across triangle edges (unlike the
// per-facet depth normal, whose steps printed every triangle edge on smooth-shaded
// legs and the terrain grid) and free of normal-map tilt (no bump self-occlusion).
vec3 octDecode(vec2 e)
{
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    float t = max(-n.z, 0.0);
    n.xy += vec2(n.x >= 0.0 ? -t : t, n.y >= 0.0 ? -t : t);
    return normalize(n);
}
vec3 srcNormal(vec2 e)
{
    const vec3 n = octDecode(e);
    return kAoGbufNormals ? normalize(mat3(ssr.views[CLEAN_VIEW].cur_view) * n) : n;
}
vec3 viewNormal(vec2 tc) { return srcNormal(textureLod(uNormal, tc, 0.0).xy); }
// (Only read where depth has a surface: (0, 0) is a valid encoding, +z / camera-facing.)
vec3 vertexNormal(vec2 tc) { return srcNormal(textureLod(uNormal, tc, 0.0).zw); }

// Face normal from depth (as HBAO/GTAO): per screen axis the neighbour with the smaller
// view-depth step is the tangent (robust at silhouettes), the two tangents are crossed
// and the result faces the camera. Returns the smooth normal where an axis has no valid
// neighbour (sky / screen edge both sides). Neighbours are one AO-resolution texel away
// (pc.screen.zw), the same grid the depth is sampled on.
vec3 geomNormal(vec2 tc, vec3 pos, vec3 smoothN)
{
    const vec2 t = pc.screen.zw;
    const float dL = depthAt(tc - vec2(t.x, 0.0)), dR = depthAt(tc + vec2(t.x, 0.0));
    const float dD = depthAt(tc - vec2(0.0, t.y)), dU = depthAt(tc + vec2(0.0, t.y));
    const bool okL = dL > 0.0, okR = dR > 0.0, okD = dD > 0.0, okU = dU > 0.0;
    if (!(okL || okR) || !(okD || okU)) return smoothN;
    const vec3 pL = viewPos(tc - vec2(t.x, 0.0), dL), pR = viewPos(tc + vec2(t.x, 0.0), dR);
    const vec3 pD = viewPos(tc - vec2(0.0, t.y), dD), pU = viewPos(tc + vec2(0.0, t.y), dU);
    const bool useR = okR && (!okL || abs(pR.z - pos.z) < abs(pL.z - pos.z));
    const bool useU = okU && (!okD || abs(pU.z - pos.z) < abs(pD.z - pos.z));
    const vec3 tx = useR ? pR - pos : pos - pL;
    const vec3 ty = useU ? pU - pos : pos - pD;
    vec3 n = cross(tx, ty);
    const float l2 = dot(n, n);
    if (l2 < 1e-20) return smoothN;
    n *= inversesqrt(l2);
    return dot(n, pos) > 0.0 ? -n : n;   // face the camera (view space: camera at origin)
}

// ---------------------------------------------------------------------------------
// HALF-RESOLUTION GRID (mode.x + 32, RenderVKAOHalfRes; see the header)
// ---------------------------------------------------------------------------------
ivec2 halfSize() { return (ivec2(pc.screen.xy) + 1) / 2; }

// The representative full-res pixel of half-res texel q and its depth (reversed-Z; 0 = sky):
// even checkerboard blocks the closest of the 2 x 2, odd ones the farthest non-sky (all
// sky: the top-left, d = 0). One gather of the block (NEAREST / CLAMP depth sampler: at an
// odd screen edge the outside texels repeat the edge texel, and the clamped pixel is the one
// whose value was read). Ties: top-left, top-right, bottom-left, bottom-right order.
// Deterministic: the kernel, both blurs and every frame pick the same pixel.
ivec2 repPixel(ivec2 q, out float d)
{
    // textureGather order: (0, 1), (1, 1), (1, 0), (0, 0) of the 2 x 2 at 2q.
    const vec4 g = textureGather(uDepth, vec2(2 * q + 1) * pc.screen.zw, 0);
    const float z[4] = float[4](g.w, g.z, g.x, g.y);   // (0,0) (1,0) (0,1) (1,1)
    const bool closest = ((q.x ^ q.y) & 1) == 0;
    int bi = 0;
    float bd = z[0];
    for (int k = 1; k < 4; ++k)
    {
        const float zk = z[k];
        const bool better = closest ? zk > bd                                   // reversed-Z: larger = nearer
                                    : (zk > 0.0 && (bd <= 0.0 || zk < bd));     // farthest surface
        if (better) { bd = zk; bi = k; }
    }
    d = bd;
    return min(2 * q + ivec2(bi & 1, bi >> 1), ivec2(pc.screen.xy) - 1);
}

// The representative of q as a view position (false: sky), and its full-res pixel.
bool repPos(ivec2 q, out ivec2 fp, out vec3 pos)
{
    float d;
    fp = repPixel(q, d);
    pos = vec3(0.0);
    if (d <= 0.0) return false;
    pos = viewPos((vec2(fp) + 0.5) * pc.screen.zw, d);
    return true;
}

vec2 octEncode(vec3 n)
{
    n /= abs(n.x) + abs(n.y) + abs(n.z);
    const vec2 e = n.xy;
    return n.z >= 0.0 ? e : (1.0 - abs(e.yx)) * vec2(e.x >= 0.0 ? 1.0 : -1.0, e.y >= 0.0 ? 1.0 : -1.0);
}

// uMeta texel of a representative (tc its centre, d its depth): the plane of its SMOOTH
// vertex normal (no normal-map tilt: the surface itself) through its exact position. View
// space and world space give the same offset (the view is a rotation about the eye), so the
// offset is dot(n_view, P_view) and the normal is stored in world space for the consumers.
vec4 aoMeta(vec2 tc, float d)
{
    if (d <= 0.0) return vec4(0.0, -1.0, 0.0, 0.0);
    const vec3 pos = viewPos(tc, d);
    const vec3 nv = vertexNormal(tc);
    const vec3 nw = normalize(transpose(mat3(ssr.views[CLEAN_VIEW].cur_view)) * nv);
    const uvec2 qn = uvec2(clamp(octEncode(nw) * 0.5 + 0.5, vec2(0.0), vec2(1.0)) * 4095.0 + 0.5);
    return vec4(dot(nv, pos), float(qn.x * 4096u + qn.y), 0.0, 0.0);
}

#endif // CLEAN_AO_COMMON_GLSL
