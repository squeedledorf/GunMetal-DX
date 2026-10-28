// vkclean -- CLUSTER LOD shared GPU contract (#642 / #743). Mirrors
// indra/newview/llcleanclusterformat.h field by field (std430); prose in
// indra/newview/CLUSTER_LOD_CONTRACT.md. Change both together and bump kFormatVersion.
//
// Method: published material only (SIGGRAPH 2021 Nanite talk; meshoptimizer's MIT
// demo/nanite.cpp boundsError projection, MIT licence).
//
// THE CLUSTER DESCRIPTOR SET (LLCleanClusterVK owns the layout; one set per frame slot):
//   set index = CLUSTER_SET: 0 in the module's own compute pipelines, 2 in cluster draw
//   pipelines (layout {mDslDraw0, mDslDraw1, cluster}); define it before including.
//   binding                      type     owner (allocates)  writers                 readers
//   0  CL_B_POOL        uint[]   SSBO     1c                 1c (transfer)           1b, 2a, 2b, draw VS / mesh
//   1  CL_B_PAGE_TABLE  uint[]   SSBO     1c                 1c (transfer)           1b
//   2  CL_B_MESHES      ClMesh[] SSBO     1c                 1c (transfer)           1b, 2a, 2b
//   3  CL_B_GROUPS      ClGroup[] SSBO    1c                 1c (transfer)           1b, 2b
//   4  CL_B_REQUESTS    hdr+req  SSBO     1c                 1b shaders (append), 1c (reset)   1c (readback)
//   5  CL_B_PAGE_USAGE  uint[]   SSBO     1c                 1b shaders              1c (readback)
//   6  CL_B_INSTANCES   ClInstance[] SSBO 1b                 1b (host / transfer)    1b, 2a, 2b
//   7  CL_B_VIEWS       ClView[] SSBO     1b                 1b (host)               1b, 2a, 2b, 2c
//   8  CL_B_SELECTED    ClSelected[] SSBO 1b                 1b selection            1b emission / draw, 2a, 2b
//   9  CL_B_COUNTERS    uint[]   SSBO     1b                 1b                      1b, 2a, 2b (layout: 1b)
//   10 CL_B_DRAWS       uint[]   SSBO+INDIRECT 1b            1b emission             indirect draw / mesh tasks
//   11 CL_B_STATS       uint[]   SSBO     core               everyone (atomicAdd)    core (readback)
//   12 CL_B_JOINT_STRETCH        SSBO     2a (core dummy)    2a                      1b, 2b
//   13 CL_B_HIZ         sampler2D         renderer (imported, PARTIALLY_BOUND)       1b
//   14 CL_B_PICK        uint[]   SSBO+INDIRECT 1b            1b pick filter          indirect pick draw (1b)
//   15 CL_B_INST_CUT    uint[]   SSBO     1b                 1b selection (main)     2b (#1011, clean_cluster_select_common.glsl)
//   14, 15 reserved for wave 1 (1b / 1c spare); 16..23 reserved for wave 2 (2a / 2b / 2c).
//   Every number 0..23 exists in the layout (placeholder storage buffers, PARTIALLY_BOUND):
//   claiming a reserved one needs only setBuffer() and a CL_B_* define here.
#ifndef CLEAN_CLUSTER_COMMON_GLSL
#define CLEAN_CLUSTER_COMMON_GLSL

#ifndef CLUSTER_SET
#define CLUSTER_SET 0
#endif

// ---- constants (= llcleanclusterformat.h) --------------------------------------------
#define CL_FORMAT_VERSION        3u
#define CL_MAX_CLUSTER_VERTS     64u
#define CL_MAX_CLUSTER_TRIS      128u
#define CL_PAGE_BYTES            81920u
#define CL_PAGE_WORDS            20480u
#define CL_PAGE_VERTS            4096u      // QVertex (20 B) units per page
#define CL_PAGE_WEIGHT_RECS      8192u      // QWeight (10 B) units per page
#define CL_PAGE_HEADER_BYTES     32u
#define CL_CLUSTER_RECORD_BYTES  64u
#define CL_MAX_CLUSTERS_PER_PAGE 64u
#define CL_CLUSTER_INDEX_BITS    6u
#define CL_MAX_VIEWS             16u
#define CL_NONE                  0xFFFFFFFFu
#define CL_GROUP_LEAF            0u
#define CL_GROUP_ROOT            1u
#define CL_ERROR_INF             3.0e38
#define CL_QHEADER_VERTS         2u

// ClusterRecord::flags
#define CLF_RIGGED        1u
#define CLF_HAS_TANGENT   2u
#define CLF_BORDER_LOCKED 8u
// ClusterInstance::flags
#define CLI_RIGGED     1u
#define CLI_SELF       2u
#define CLI_NO_REQUEST 4u
// ClusterView::flags
#define CLV_ORTHO     1u
#define CLV_OCCLUSION 2u
#define CLV_REQUESTS  4u
#define CLV_RT        8u
#define CLV_SHADOW    16u
#define CLV_PROBE     32u
#define CLV_MAIN      64u
#define CLV_NO_CONE   128u
#define CLV_HIDE_SELF 256u   // main view: CLI_SELF instances selected (rig lists / RT) but not drawn / picked
#define CLV_NO_RIGGED 512u   // <GMDX> avatars render type off: no rigged instance is selected in any view
#define CLV_AVATARS_ONLY 1024u   // <GMDX> main view, avatar matte snapshot: statics selected but not drawn
// Stats counter indices
#define CLS_INSTANCES      0u
#define CLS_TESTED         1u
#define CLS_CUT            2u
#define CLS_FALLBACK       3u
#define CLS_CULL_FRUSTUM   4u
#define CLS_CULL_OCCLUSION 5u
#define CLS_CULL_CONE      6u
#define CLS_SELECTED       7u
#define CLS_SEL_OVERFLOW   8u
#define CLS_TRIANGLES      9u
#define CLS_VERTICES      10u
#define CLS_REQUESTS      11u
#define CLS_REQ_OVERFLOW  12u
#define CLS_DRAWS         13u
#define CLS_MAIN_INST     14u   // main view only (CLV_MAIN), kFormatVersion 3
#define CLS_MAIN_ROOT     15u
#define CLS_MAIN_CUT_TRIS 16u
#define CLS_MAIN_FULL_TRIS 17u
#define CLS_MAIN_SEL_TRIS 18u
#define CLS_COUNT         32u
// ClusterGroup::flags
#define CLG_FACE_BORDER_FREE 1u

// ---- bindings ---------------------------------------------------------------------------
#define CL_B_POOL          0
#define CL_B_PAGE_TABLE    1
#define CL_B_MESHES        2
#define CL_B_GROUPS        3
#define CL_B_REQUESTS      4
#define CL_B_PAGE_USAGE    5
#define CL_B_INSTANCES     6
#define CL_B_VIEWS         7
#define CL_B_SELECTED      8
#define CL_B_COUNTERS      9
#define CL_B_DRAWS         10
#define CL_B_STATS         11
#define CL_B_JOINT_STRETCH 12
#define CL_B_HIZ           13
#define CL_B_PICK          14   // 1b (#702 cluster pick): the GPU pick's cluster draw list (count + tier-0 records)
#define CL_B_INST_CUT      15   // 1b (#1011): per instance, the main view's skinned cut summary (2b's derived RT cut)
#define CL_B_SKIN_OUT      16   // 2a (#747): skinned cluster arena (clean_cluster_skin_common.glsl)
#define CL_B_SKIN_SCRATCH  17   // 2a (#747): skin scratch: header, margins, map, weld

// ---- records (std430; sizes asserted on the C++ side) ------------------------------------
struct ClRecord          // 64 B, read from the pool words (clLoadRecord)
{
    vec4  sphere;        // MESH space tight bounds
    uvec4 joint_mask;
    uint  vert_off20;    // first real vertex, page offset / 20
    uint  counts;        // vert | tri << 8 | level << 16 | anchor << 24
    uint  index_off4;
    uint  weight_off10;  // CL_NONE = none
    uint  group;         // containing group (mesh-local)
    uint  child_group;   // generating group (mesh-local)
    uint  cone;          // snorm8 axis xyz + cutoff
    uint  flags;
};
struct ClGroup           // 48 B
{
    vec4  sphere;        // LOD sphere, MESH space
    uvec4 joint_mask;
    float error;         // MESH-space metres
    uint  page;          // mesh-local page of all members (CL_NONE: sentinel)
    uint  counts;        // members | level << 8 | anchor << 16
    uint  flags;         // CLG_*
};
struct ClMesh            // 96 B
{
    vec4  qh0;           // pos_c.xyz, pos_s.x
    vec4  qh1;           // pos_s.yz, uv_c.xy
    vec4  qh2;           // uv_s.xy, z = LOD0 triangle count (uint bits: clMeshLod0Tris), w 0
    vec4  sphere;        // MESH space
    uint  page_base;     // virtual page id of page 0
    uint  page_count;
    uint  root_pages;
    uint  group_base;
    uint  group_count;
    uint  cluster_count;
    uint  flags;
    uint  generation;
};
struct ClInstance        // 80 B
{
    vec4  xform_r0;      // 3x4 row-major mesh -> world
    vec4  xform_r1;
    vec4  xform_r2;
    uint  render_instance;
    uint  mesh;
    uint  skin_id;
    uint  flags;
    float tol_scale;
    float lod_scale;
    float anim_margin_m;
    uint  mesh_generation;
};
struct ClView            // 256 B
{
    mat4  view_proj;     // (world - origin) -> clip
    vec4  planes[6];     // world space, inside >= 0
    vec4  cam_pos;       // xyz, w = znear
    vec4  origin;
    float px_scale;
    float tol_px;
    uint  flags;
    uint  sel_base;
    uint  sel_cap;
    uint  draw_base;
    uint  draw_cap;
    uint  hiz_size;
    uint  frame;
    uint  view_index;
    vec2  viewport_px;
    vec4  lod_cam;       // xyz LOD camera (error projection only), w its distance margin (#749)
};
// The face's LOD0 triangle count (= LLClusterFmt::meshLod0Tris; the stats' full-detail baseline).
uint clMeshLod0Tris(ClMesh m) { return floatBitsToUint(m.qh2.z); }
struct ClSelected { uint cluster_ref; uint instance; };
struct ClRequest  { uint vpage; float priority; };

// ---- optional declarations (define CL_DECLARE_<NAME> before including; qualifiers may be
// overridden, e.g. #define CL_REQUESTS_QUAL coherent) ----------------------------------------
#ifndef CL_READONLY
#define CL_READONLY readonly
#endif
#ifdef CL_DECLARE_POOL
layout(std430, set = CLUSTER_SET, binding = CL_B_POOL) CL_READONLY buffer ClPool { uint cl_pool[]; };
#endif
#ifdef CL_DECLARE_PAGE_TABLE
layout(std430, set = CLUSTER_SET, binding = CL_B_PAGE_TABLE) CL_READONLY buffer ClPageTable { uint cl_page_table[]; };
#endif
#ifdef CL_DECLARE_MESHES
layout(std430, set = CLUSTER_SET, binding = CL_B_MESHES) CL_READONLY buffer ClMeshes { ClMesh cl_meshes[]; };
#endif
#ifdef CL_DECLARE_GROUPS
layout(std430, set = CLUSTER_SET, binding = CL_B_GROUPS) CL_READONLY buffer ClGroups { ClGroup cl_groups[]; };
#endif
#ifdef CL_DECLARE_REQUESTS
#ifndef CL_REQUESTS_QUAL
#define CL_REQUESTS_QUAL
#endif
layout(std430, set = CLUSTER_SET, binding = CL_B_REQUESTS) CL_REQUESTS_QUAL buffer ClRequests
{
    uint cl_req_count; uint cl_req_cap; uint cl_req_frame; uint cl_req_pad;
    ClRequest cl_requests[];
};
#endif
#ifdef CL_DECLARE_PAGE_USAGE
layout(std430, set = CLUSTER_SET, binding = CL_B_PAGE_USAGE) buffer ClPageUsage { uint cl_page_usage[]; };
#endif
#ifdef CL_DECLARE_INSTANCES
layout(std430, set = CLUSTER_SET, binding = CL_B_INSTANCES) CL_READONLY buffer ClInstances { ClInstance cl_instances[]; };
#endif
#ifdef CL_DECLARE_VIEWS
layout(std430, set = CLUSTER_SET, binding = CL_B_VIEWS) CL_READONLY buffer ClViews { ClView cl_views[]; };
#endif
#ifdef CL_DECLARE_STATS
layout(std430, set = CLUSTER_SET, binding = CL_B_STATS) buffer ClStats { uint cl_stats[]; };
#define CL_STAT_ADD(I, N) atomicAdd(cl_stats[(I)], (N))
#else
#define CL_STAT_ADD(I, N)
#endif
#ifdef CL_DECLARE_JOINT_STRETCH
// rows = skins with a stretch row (0: every stretch is 1), then rows x 128 floats.
layout(std430, set = CLUSTER_SET, binding = CL_B_JOINT_STRETCH) readonly buffer ClJointStretch
{
    uint cl_js_rows; uint cl_js_pad0; uint cl_js_pad1; uint cl_js_pad2;
    float cl_joint_stretch[];
};
#endif
#ifdef CL_DECLARE_HIZ
layout(set = CLUSTER_SET, binding = CL_B_HIZ) uniform sampler2D cl_hiz;
#endif

// ---- pool addressing -------------------------------------------------------------------
uint clRefSlot(uint ref)  { return ref >> CL_CLUSTER_INDEX_BITS; }
uint clRefIndex(uint ref) { return ref & ((1u << CL_CLUSTER_INDEX_BITS) - 1u); }
uint clMakeRef(uint slot, uint idx) { return (slot << CL_CLUSTER_INDEX_BITS) | idx; }
// Word offset of a cluster record in the pool.
uint clRecordWord(uint slot, uint idx)
{
    return slot * CL_PAGE_WORDS + (CL_PAGE_HEADER_BYTES + idx * CL_CLUSTER_RECORD_BYTES) / 4u;
}
uint clRecVertCount(ClRecord r) { return r.counts & 0xFFu; }
uint clRecTriCount(ClRecord r)  { return (r.counts >> 8u) & 0xFFu; }
uint clRecLevel(ClRecord r)     { return (r.counts >> 16u) & 0xFFu; }
uint clRecAnchor(ClRecord r)    { return (r.counts >> 24u) & 0x7Fu; }
// ABSOLUTE addresses of a resident cluster's data, in the units the existing consumers use:
// first real vertex (QVertex index; header at -2: QV_LOAD_HEADER / src_vert_base / vertexOffset),
// first weight record (QWeight index: clean_preskin src_weight_base), first index BYTE
// (uint8 index draw firstIndex).
uint clFirstVertex(uint slot, ClRecord r) { return slot * CL_PAGE_VERTS + r.vert_off20; }
uint clFirstWeight(uint slot, ClRecord r)
{
    return r.weight_off10 == CL_NONE ? CL_NONE : slot * CL_PAGE_WEIGHT_RECS + r.weight_off10;
}
uint clFirstIndexByte(uint slot, ClRecord r) { return slot * CL_PAGE_BYTES + r.index_off4 * 4u; }

#ifdef CL_DECLARE_POOL
ClRecord clLoadRecord(uint slot, uint idx)
{
    const uint w = clRecordWord(slot, idx);
    ClRecord r;
    r.sphere = uintBitsToFloat(uvec4(cl_pool[w], cl_pool[w + 1u], cl_pool[w + 2u], cl_pool[w + 3u]));
    r.joint_mask = uvec4(cl_pool[w + 4u], cl_pool[w + 5u], cl_pool[w + 6u], cl_pool[w + 7u]);
    r.vert_off20 = cl_pool[w + 8u];
    r.counts = cl_pool[w + 9u];
    r.index_off4 = cl_pool[w + 10u];
    r.weight_off10 = cl_pool[w + 11u];
    r.group = cl_pool[w + 12u];
    r.child_group = cl_pool[w + 13u];
    r.cone = cl_pool[w + 14u];
    r.flags = cl_pool[w + 15u];
    return r;
}
// Cluster count of the page in a pool slot (page header word 1, bits 0..7).
uint clPageClusterCount(uint slot) { return cl_pool[slot * CL_PAGE_WORDS + 1u] & 0xFFu; }
// Local index i (0 .. 3 * tri_count - 1) of a resident cluster: a uint8 read from the words.
uint clLocalIndex(uint slot, ClRecord r, uint i)
{
    const uint b = clFirstIndexByte(slot, r) + i;
    return (cl_pool[b >> 2u] >> ((b & 3u) * 8u)) & 0xFFu;
}
#endif

// ---- page table --------------------------------------------------------------------------
#ifdef CL_DECLARE_PAGE_TABLE
// Pool slot of a virtual page, CL_NONE when not resident.
uint clLookupPage(uint vpage) { return cl_page_table[vpage]; }
#ifdef CL_DECLARE_MESHES
// Pool slot of a mesh-local page (CL_NONE = not resident or out of range).
uint clLookupMeshPage(uint mesh, uint local_page)
{
    if (local_page >= cl_meshes[mesh].page_count) return CL_NONE;
    return cl_page_table[cl_meshes[mesh].page_base + local_page];
}
#ifdef CL_DECLARE_GROUPS
// A group's members are resident (sentinels: always "resident").
bool clGroupResident(uint mesh, uint group)
{
    const uint p = cl_groups[cl_meshes[mesh].group_base + group].page;
    return p == CL_NONE || clLookupMeshPage(mesh, p) != CL_NONE;
}
#endif
#endif
#endif

// ---- joint stretch -----------------------------------------------------------------------
#ifdef CL_DECLARE_JOINT_STRETCH
// Upper bound of the joints' extra stretch over rest pose (>= 1) for a joint mask.
float clJointStretch(uint skin_id, uvec4 mask)
{
    if (skin_id == CL_NONE || skin_id >= cl_js_rows) return 1.0;
    float s = 1.0;
    for (uint w = 0u; w < 4u; ++w)
    {
        uint m = mask[w];
        while (m != 0u)
        {
            const uint b = uint(findLSB(m));
            m &= m - 1u;
            s = max(s, cl_joint_stretch[skin_id * 128u + w * 32u + b]);
        }
    }
    return s;
}
#else
float clJointStretch(uint skin_id, uvec4 mask) { return 1.0; }
#endif

// ---- projection + cut rule (= LLClusterFmt::projectGroupError / clusterRenders) -----------
// Mesh-space point / sphere to world.
vec3 clXformPoint(ClInstance inst, vec3 p)
{
    const vec4 h = vec4(p, 1.0);
    return vec3(dot(inst.xform_r0, h), dot(inst.xform_r1, h), dot(inst.xform_r2, h));
}
vec4 clXformSphere(ClInstance inst, vec4 s) { return vec4(clXformPoint(inst, s.xyz), s.w * inst.lod_scale); }

// Projected error (px) of a GROUP for an instance in a view. stretch = clJointStretch(...)
// of the GROUP's mask. Kept arithmetic-identical to the C++ reference.
float clProjectGroupError(vec4 sphere, float error, ClInstance inst, ClView v, float stretch)
{
    if (error >= CL_ERROR_INF) return CL_ERROR_INF;
    if (error <= 0.0) return 0.0;
    const vec4 c = clXformSphere(inst, sphere);
    const float err_w = error * inst.lod_scale * stretch;
    if ((v.flags & CLV_ORTHO) != 0u) return err_w * v.px_scale;
    const float d = length(c.xyz - v.lod_cam.xyz) - c.w - inst.anim_margin_m - v.lod_cam.w;
    return err_w * v.px_scale / max(d, v.cam_pos.w);
}
// THE cut rule: parent_px = proj(group(C)), self_px = proj(child_group(C)),
// child_resident = clGroupResident(mesh, child_group(C)).
bool clCutRenders(float parent_px, float self_px, bool child_resident, float tol)
{
    return parent_px > tol && (self_px <= tol || !child_resident);
}
// Tolerance of an instance in a view.
float clTolerance(ClInstance inst, ClView v) { return v.tol_px * inst.tol_scale; }

// ---- culling helpers ---------------------------------------------------------------------
// World sphere vs the view's planes (true = at least partly inside).
bool clSphereInFrustum(ClView v, vec4 s)
{
    for (uint i = 0u; i < 6u; ++i)
        if (dot(v.planes[i].xyz, s.xyz) + v.planes[i].w < -s.w) return false;
    return true;
}
// Backface cone (meshopt cone_axis_s8 / cone_cutoff_s8, MESH space). Returns true when the
// whole cluster faces away from cam (mesh space camera position; caller transforms it).
bool clConeBackfacing(uint cone, vec3 sphere_c_mesh, float radius, vec3 cam_mesh)
{
    const vec4 c = vec4(ivec4(int(cone << 24u) >> 24, int(cone << 16u) >> 24, int(cone << 8u) >> 24, int(cone) >> 24)) / 127.0;
    if (c.w >= 1.0) return false;   // cutoff 127: no usable cone
    const vec3 d = sphere_c_mesh - cam_mesh;
    return dot(d, c.xyz) >= c.w * length(d) + radius;
}

#endif
