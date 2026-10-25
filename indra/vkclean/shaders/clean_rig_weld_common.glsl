// vkclean -- RIGGED WELD PAIRING, shared by BOTH welds (#574 / #641 / #626 / #798): the legacy
// weld (clean_rig_weld.comp: per-frame search mode 0, rest-pose pairs modes 6 / 7) and the cluster
// LOD weld on the cut (clean_cluster_preskin.comp insert, clean_cluster_preskin_weld.comp resolve).
// One copy of the pairing rules so the two paths cannot diverge again (#798: the cluster weld
// paired in BIND space -- legacy mode 0 -- while the legacy default, mode 1, pairs at the avatar's
// REST pose: meshes whose inv_bind matrices differ but that meet on this avatar's shape were
// welded on the legacy path and left open on the cluster path).
//
// The includer defines, BEFORE including (an includer without them -- the cluster weld resolve --
// gets the hash and the overlap only):
//   RW_META(w)  the uint word `w` of the skin meta buffer (LLCleanSkinStore::SkinMeta: per skin
//               meta_words words; 0 avatar_idx, 1 joint_count, 4.. joint_nums[110], 116..
//               inv_bind[110] (mat4, column-major), 1876.. bind_shape)
//   RW_REST(w)  the uint word `w` of the buffer holding the REST JOINT ROWS (216 joints x 8 words
//               {pos xyz, -, scale xyz, -}, float bits: llcleanrigweldvk.cpp LLRigRest::buildRow --
//               identity joint rotations, the shape's joint positions / scales and the joint
//               overrides; the root at the origin)
#ifndef CLEAN_RIG_WELD_COMMON_GLSL
#define CLEAN_RIG_WELD_COMMON_GLSL

#define RW_META_JOINT_COUNT 1u
#define RW_META_JOINT_NUMS  4u
#define RW_META_INV_BIND    116u
#define RW_META_BIND_SHAPE  1876u
#define RW_REST_JOINTS      216u
#define RW_REST_ROW_WORDS   (RW_REST_JOINTS * 8u)   // = LLRigRest::kRowWords

// The weld grid's cell hash: cell c of avatar `avatar` -> a bucket (mask = cells - 1).
uint rwCellHash(ivec3 c, uint avatar, uint mask)
{
    uint h = uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u ^ avatar * 2654435761u;
    h ^= h >> 15u;
    h *= 0x2c1b3c6du;
    h ^= h >> 12u;
    return h & mask;
}

// ---- THE RESOLVE (#798): one rule for legacy mode 1's GATHER and the cluster weld ----------------
// Per candidate: its own skinned position / normal + ONE PARTNER PER OTHER MESH (that mesh's
// nearest candidate in the pairing space, legacy PAIR's rule). A partner farther than max_gap in
// this pose is left out (meshes that move apart for real); the rest are averaged symmetrically:
// position = mean(own, partners), normal = normalize(own unit + each partner's unit normal within
// cos_n of own -- a harder edge keeps its own normal), the tangent re-orthogonalised by APPLY.
//
// NO-OP (#798): when the blend's RESULT equals the vertex within RW_NOOP_DIST / RW_NOOP_COS,
// nothing is written: the weld closes gaps, it never re-shades geometry the creator made seamless
// (writing an unchanged vertex still re-quantises its normal and re-orthogonalises its tangent --
// only on the vertices that found a partner, while their neighbours along the edge keep theirs: a
// line along an already seamless seam). Result-based, so it covers seamless partners, partners
// whose normal is past the angle (a deliberate hard edge between coincident meshes: the blend keeps
// the own normal) and mixtures; symmetric -- a partner that moves the blend moves it on both sides.
//   RW_NOOP_DIST 50 um: two meshes' vertices are quantised on their own lattices (QVertex 16-bit
//     positions: a step of extent / 65534, ~27 um for a 1.8 m body face; up to one step apart after
//     both round), plus the float skinning of two bind shapes; a 50 um gap is sub-pixel anywhere.
//   RW_NOOP_COS cos 2 deg: oct16 normals are ~0.005 deg; the rest is the two skins' float
//     transforms of an identical normal (identical weights) -- a creator's split edge is far wider.
#define RW_NOOP_DIST 5.0e-5
#define RW_NOOP_COS  0.99939083   // cos(2 deg)
bool rwSeamless(vec3 p0, vec3 u0, vec3 p1, vec3 u1)
{
    return distance(p0, p1) <= RW_NOOP_DIST && dot(u0, u1) >= RW_NOOP_COS;
}
struct RwBlend
{
    vec3 p0, u0, psum, nsum;
    uint used;     // partners within the max gap
    uint gap;      // partners past it
};
RwBlend rwBlendBegin(vec3 p0, vec3 n0)
{
    RwBlend b;
    const float l = length(n0);
    b.p0 = p0;
    b.u0 = l > 1e-12 ? n0 / l : vec3(0.0, 0.0, 1.0);
    b.psum = p0;
    b.nsum = b.u0;
    b.used = 0u;
    b.gap = 0u;
    return b;
}
// One partner (the chosen vertex of one other mesh), skinned position + normal.
void rwBlendAdd(inout RwBlend b, vec3 op, vec3 on, float max_gap, float cos_n)
{
    if (!(distance(op, b.p0) <= max_gap)) { ++b.gap; return; }
    b.psum += op;
    ++b.used;
    const float ln = length(on);
    const vec3 un = ln > 1e-12 ? on / ln : b.u0;
    if (ln > 1e-12 && dot(un, b.u0) >= cos_n) b.nsum += un;
}
// The result: 1 = write (p, nn), 0 = nothing to do (no partner in range), 2 = NO-OP (the blend
// equals the vertex within the thresholds: nothing written).
uint rwBlendEnd(RwBlend b, out vec3 p, out vec3 nn)
{
    p = b.p0;
    nn = b.u0;
    if (b.used == 0u) return 0u;
    p = b.psum / float(b.used + 1u);
    const float nl = length(b.nsum);
    nn = nl > 1e-12 ? b.nsum / nl : b.u0;
    if (any(isnan(p)) || any(isinf(p)) || any(isnan(nn)) || any(isinf(nn))) { p = b.p0; nn = b.u0; return 0u; }
    if (rwSeamless(b.p0, b.u0, p, nn)) { p = b.p0; nn = b.u0; return 2u; }
    return 1u;
}

// Weights (sum 1) -> 4 x 16 bits, as the grid entries store them.
uvec2 rwPackWeights(vec4 w)
{
    const uvec4 q = uvec4(round(clamp(w, vec4(0.0), vec4(1.0)) * 65535.0));
    return uvec2(q.x | (q.y << 16u), q.z | (q.w << 16u));
}

// #626 skin-weight overlap of two candidates (resolved avatar joints, 8 bits each; packed weights):
// sum over the distinct avatar joints of min(w_a[j], w_b[j]) -- 1 = identical deformation, 0 =
// disjoint. Cavity rims / onion-skin layers weighted to other joints never pair.
float rwWeightOverlap(uint ja, uvec2 qa, uint jb, uvec2 qb)
{
    const vec4 wa = vec4(float(qa.x & 0xFFFFu), float(qa.x >> 16u), float(qa.y & 0xFFFFu), float(qa.y >> 16u)) * (1.0 / 65535.0);
    const vec4 wb = vec4(float(qb.x & 0xFFFFu), float(qb.x >> 16u), float(qb.y & 0xFFFFu), float(qb.y >> 16u)) * (1.0 / 65535.0);
    float s = 0.0;
    for (uint i = 0u; i < 4u; ++i)
    {
        if (wa[i] <= 0.0) continue;
        const uint j = (ja >> (i * 8u)) & 0xFFu;
        // a joint listed twice on one side counts once, with its weights summed
        bool seen = false;
        float sa = 0.0;
        for (uint k = 0u; k < 4u; ++k)
        {
            if (((ja >> (k * 8u)) & 0xFFu) != j) continue;
            if (k < i && wa[k] > 0.0) seen = true;
            sa += wa[k];
        }
        if (seen) continue;
        float sb = 0.0;
        for (uint k = 0u; k < 4u; ++k) if (((jb >> (k * 8u)) & 0xFFu) == j) sb += wb[k];
        s += min(sa, sb);
    }
    return s;
}

#ifdef RW_META
// The skin meta's lane -> joint-table index map: palette lane `lane` of skin (meta word base mb)
// is avatar joint rwJoint(..) (lanes past joint_count repeat the last real one, as
// clean_skin_palette.comp does).
uint rwJoint(uint mb, uint lane)
{
    const uint jc = RW_META(mb + RW_META_JOINT_COUNT);
    const uint src_j = jc > 0u ? min(min(lane, 109u), jc - 1u) : 0u;
    return uint(clamp(int(RW_META(mb + RW_META_JOINT_NUMS + src_j)), 0, 215));
}
// The 4 influences' resolved avatar joints, 8 bits each (the #626 overlap's joint ids).
uint rwJoints(uint mb, uvec4 lanes)
{
    uint ja = 0u;
    for (uint k = 0u; k < 4u; ++k) ja |= rwJoint(mb, lanes[k]) << (k * 8u);
    return ja;
}

mat4 rwMetaMat(uint w)
{
    return mat4(uintBitsToFloat(RW_META(w + 0u)),  uintBitsToFloat(RW_META(w + 1u)),  uintBitsToFloat(RW_META(w + 2u)),  uintBitsToFloat(RW_META(w + 3u)),
                uintBitsToFloat(RW_META(w + 4u)),  uintBitsToFloat(RW_META(w + 5u)),  uintBitsToFloat(RW_META(w + 6u)),  uintBitsToFloat(RW_META(w + 7u)),
                uintBitsToFloat(RW_META(w + 8u)),  uintBitsToFloat(RW_META(w + 9u)),  uintBitsToFloat(RW_META(w + 10u)), uintBitsToFloat(RW_META(w + 11u)),
                uintBitsToFloat(RW_META(w + 12u)), uintBitsToFloat(RW_META(w + 13u)), uintBitsToFloat(RW_META(w + 14u)), uintBitsToFloat(RW_META(w + 15u)));
}

#ifdef RW_REST
// THE PAIRING SPACE (#641): a bind-space point (bind_shape x vertex) of skin `mb` (meta word base)
// skinned at the avatar's REST pose (rest row at word `rest`): lane j of the rest palette =
// restWorld(joint_nums[j]) x inv_bind[j], restWorld = translate(pos) x scale(scale)
// (LLMatrix4::initAll, identity rotation). Zero-weight influences are skipped; weights sum to 1
// (normalised as the pre-skins do). Two meshes of one avatar whose vertices meet on THIS avatar's
// shape meet here, whatever their bind poses.
vec3 rwRestPos(uint mb, uint rest, vec3 pb, uvec4 lanes, vec4 w)
{
    vec3 p = vec3(0.0);
    const uint jc = RW_META(mb + RW_META_JOINT_COUNT);
    for (uint k = 0u; k < 4u; ++k)
    {
        if (w[k] == 0.0) continue;
        const uint lane = min(lanes[k], 109u);
        const uint src_j = jc > 0u ? min(lane, jc - 1u) : 0u;
        const uint a = rwJoint(mb, lanes[k]);
        const vec3 q = (rwMetaMat(mb + RW_META_INV_BIND + src_j * 16u) * vec4(pb, 1.0)).xyz;
        const uint rj = rest + a * 8u;
        const vec3 jt = vec3(uintBitsToFloat(RW_REST(rj + 0u)), uintBitsToFloat(RW_REST(rj + 1u)), uintBitsToFloat(RW_REST(rj + 2u)));
        const vec3 js = vec3(uintBitsToFloat(RW_REST(rj + 4u)), uintBitsToFloat(RW_REST(rj + 5u)), uintBitsToFloat(RW_REST(rj + 6u)));
        p += w[k] * (js * q + jt);
    }
    return p;
}
#endif // RW_REST
#endif // RW_META

#endif
