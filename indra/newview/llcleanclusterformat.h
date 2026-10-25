/**
 * @file llcleanclusterformat.h
 * @brief Cluster LOD (#642 / #743): THE shared data contract -- cluster limits, the GPU
 *        records (std430), the fixed-size page layout, the DAG / group records, the cut
 *        rule, error units, the joint-stretch bound and the on-disk file format.
 *
 * Every module of the cluster LOD path (ingest 1a, selection / emission 1b, streaming 1c,
 * skinning 2a, RT 2b, controller 2c) reads its shared structs from HERE and from the GLSL
 * mirror indra/vkclean/shaders/clean_cluster_common.glsl. Change one, change both, bump
 * kFormatVersion (GPU / page layout) or kFileVersion (disk). Prose: CLUSTER_LOD_CONTRACT.md.
 *
 * Pure C++ (no Vulkan, no LL types): included by CPU workers, the disk cache and vkgfx.
 *
 * Method: published material only -- the cluster DAG approach as presented publicly
 * (SIGGRAPH 2021 "A Deep Dive into Nanite Virtualized Geometry") and meshoptimizer's MIT
 * demo (demo/nanite.cpp, MIT licence: group bounds merge with error = max(children),
 * boundsError projection). No engine source.
 *
 * ============================================================================================
 * CLUSTER SIZE: <= 64 vertices, <= 128 triangles (kMaxClusterVerts / kMaxClusterTris).
 *   - mesh shader (VK_EXT_mesh_shader): one 64-invocation workgroup per cluster = one vertex
 *     per invocation, two triangles per invocation. RTX 5090: maxMeshWorkGroupInvocations
 *     128, maxMeshOutputVertices / Primitives 256, maxMeshOutputMemorySize 32 KiB -- 64
 *     vertices x the G-buffer varyings (~8 vec4) is 8 KiB, 128 vertices would be ~16-29 KiB
 *     (tight against the output memory once the full world VS interface is carried).
 *   - NV cluster acceleration structures: maxVerticesPerCluster / maxTrianglesPerCluster
 *     are queried at runtime (LLCleanClusterCaps); the CLAS tier is refused unless both
 *     are >= our limits (RTX 50: 256 / 256).
 *   - uint8 local indices need <= 256 vertices (we use 6 bits of them).
 *   - pre-skin (clean_preskin.comp, local_size_x 64): one workgroup = one cluster.
 *   - meshoptimizer: meshopt_buildMeshlets* limits are 256 vertices / 512 triangles,
 *     triangles divisible by 4 (128 ok).
 *   Trade-off vs 128-vertex clusters: ~2x the cluster count (selection threads, records,
 *   DAG levels); vs NVIDIA's older 64 / 126 advice: 128 keeps meshopt's divisibility and
 *   a power-of-two triangle loop. 1a may emit smaller clusters (kMinClusterTris is a hint).
 *
 * INDEX FORMAT: uint8 LOCAL indices (0..vert_count-1), 3 per triangle, packed bytes.
 *   - mesh shader tier: read from the page pool as words, no index type involved.
 *   - indexed MDI tier: the page pool is bound as a VK_INDEX_TYPE_UINT8 index buffer
 *     (VK_KHR / EXT_index_type_uint8 or Vulkan 1.4; RTX 5090: indexTypeUint8 = true);
 *     firstIndex = the block's absolute byte offset in the pool, vertexOffset = the
 *     cluster's absolute first vertex (pool slot * kPageVerts + vert_off20).
 *   - floor tier (no uint8 index type): non-indexed MDI, the VS pulls the byte index from
 *     the pool by gl_VertexIndex (vertex pulling; core Vulkan, no extension).
 *   => the pool must stay < 4 GiB (firstIndex is uint32): kMaxPoolPages.
 *
 * PAGE: kPageBytes = 81920 (80 KiB) = 4096 QVertex (20 B) = 8192 QWeight (10 B) = 5120 x 16 B.
 *   The page pool is ONE device buffer of N pages (N chosen by 1c, <= kMaxPoolPages).
 *   Because every page base is a multiple of 20, 10 and 16, the EXISTING consumers address
 *   page data in their own units without any new decode:
 *     absolute first vertex  = pool_slot * kPageVerts      + rec.vert_off20     (QVertex units)
 *     absolute weight record = pool_slot * kPageWeightRecs + rec.weight_off10   (QWeight units)
 *     absolute index byte    = pool_slot * kPageBytes      + rec.index_off4 * 4
 *   Each cluster's vertex block is [QHeader (2 pseudo-vertices)][vert_count QVertex], so the
 *   decode header sits at first_vertex - 2 exactly as for a mega-VBO slot (clean_qvertex.glsl
 *   QV_LOAD_HEADER, clean_preskin.comp src_vert_base, clean_draw.vert gl_BaseVertex).
 *   The pool is a SEPARATE VkBuffer from the mega-VBO / weight arena: format-compatible, not
 *   the same buffer. Consumers bind the pool where they bind the mega-VBO today (a
 *   -DCLEAN_CLUSTER variant of clean_draw.vert reads its header words from the cluster set's
 *   pool binding; the cluster pre-skin set binds the pool at bindings 0 AND 1).
 *
 *   Page byte layout (offsets from the page start; the builder writes it, nobody patches it):
 *     [0, 32)                      ClusterPageHeader
 *     [32, 32 + 64 n)              ClusterRecord[n]                         n <= kMaxClustersPerPage
 *     pad to a multiple of 20
 *     vertex blocks                per cluster: QHeader (40 B) + vert_count x 20 B, back to back
 *                                  (every block starts on a multiple of 20)
 *     weight blocks                rigged pages only: per cluster vert_count x 10 B, back to back
 *                                  (start = end of the vertex blocks, a multiple of 20)
 *     index blocks                 per cluster tri_count x 3 B, each padded to 4 B
 *     zero fill to kPageBytes      (pages on disk store used_bytes; the tail is never read)
 *   Worst case (64 clusters of 32 tris / ~24 verts, or 32 full clusters of 64 v / 128 t):
 *   the builder packs WHOLE GROUPS until the next one does not fit (packPageFits below).
 *
 * QUANTIZATION LATTICE: ONE (pos_c, pos_s, uv_c, uv_s) per cluster mesh (= mesh asset + face),
 *   computed from LOD0's extent (LLCleanQV::makeHeader) and COPIED into every cluster's
 *   QHeader at every DAG level; simplified vertices are re-quantized onto it (error <= s/2,
 *   ~1.5e-5 of the extent, far below any tolerance). Locked border vertices therefore
 *   coincide bit-for-bit between clusters, groups and DAG levels -- watertight cuts.
 *
 * WEIGHTS: LLCleanQV::QWeight (10 B), unchanged. Bit 7 of j[3] (kWeldBoundaryBit) keeps its
 *   meaning "open boundary of the ORIGINAL face" (#574 rigged welding): 1a sets it by
 *   matching against LOD0's open-boundary vertex set (those vertices are locked in every
 *   simplification, so their positions survive) -- NOT by markWeldBoundary on a cluster's
 *   own triangles (that would flag every cluster seam).
 *
 * ============================================================================================
 * DAG. Clusters are grouped (kGroupTarget ~8, <= kMaxGroupClusters), each group simplified
 * with its outer border locked and re-split into clusters (the next, coarser level).
 *   group(C)        CONTAINING group: the group C is a member of (simplifying it produced
 *                   C's parents). Root clusters: kGroupRoot.
 *   child_group(C)  GENERATING group: the group whose simplification produced C. LOD0
 *                   clusters: kGroupLeaf.
 * Group ids are mesh-local; 0 and 1 are SENTINELS present in every mesh's group table:
 *   kGroupLeaf (0): error 0, sphere = the mesh sphere, page kNone.
 *   kGroupRoot (1): error kErrorInf, sphere = the mesh sphere, page kNone.
 * BUILDER GUARANTEES (1a; checked by LLClusterValidate in debug builds):
 *   (G1) monotonic error: error(G) >= error(H) for every group H that generated a member
 *        of G; error(G) = max(simplify error of G, max over those H) (meshopt demo merge).
 *   (G2) nested bounds: sphere(G) contains sphere(H) for every such H (sphere merge).
 *   (G3) joint masks nest: mask(G) is a superset of mask(H) (union) -> stretch monotonic.
 *   (G4) a group's members all live in ONE page (pages hold whole groups): group residency
 *        == page residency.
 *   (G5) pages are ordered coarse -> fine; pages [0, root_pages) hold the root level and are
 *        pinned resident while the mesh is registered.
 *   (G6) every page lists its parent pages (pages holding a cluster generated by a group in
 *        this page): 1c keeps residency CLOSED -- a page is made resident only after all its
 *        parent pages, and a page is evicted only after every page that depends on it.
 *   (G7) the face's open border (the seam to the mesh's other faces, the #574 weld set) is
 *        locked at every level EXCEPT in groups flagged CLG_FACE_BORDER_FREE (builder v2: the
 *        DAG floor, only after the locked simplification stalled), whose error is >= the
 *        group sphere's diameter -- a bound of the deviation of anything inside that sphere,
 *        so a crack to a neighbouring face is never larger than the tolerance. Group borders
 *        stay locked at every level: a cut is watertight inside the face.
 *
 * CUT RULE (per cluster instance, per view; clusterRenders below / clCutRenders in GLSL):
 *   render(C) = resident(page(C))                              (only resident clusters run)
 *             AND  proj(group(C))       >  tol                 (parent too coarse)
 *             AND (proj(child_group(C)) <= tol                 (self fine enough)
 *                  OR NOT resident(page(child_group(C))))      (finer page missing: fall back)
 * proj() of a GROUP record only (sphere + error + mask); the cluster's own tight sphere is for
 * frustum / occlusion / cone tests. Both sides of every boundary read the SAME group record
 * through the same function, so the decision is identical on both sides (watertight), and
 * (G1)-(G3) make proj monotonic up the DAG, so exactly one cut is chosen. A missing finer
 * page draws the coarser parent (never a hole): the request for it goes to 1c.
 *
 * ERROR UNITS: metres in MESH SPACE -- the space of the decoded QVertex positions of the face
 * (what LLCleanMeshAsset captured). Per instance (ClusterInstance):
 *   xform      mesh space -> world (rigid + scale; rigged: the rest-pose mapping, avatar
 *              world x bind shape; 2a may refine). One transform per instance, the same for
 *              every group of the mesh -> nesting survives the transform.
 *   lod_scale  sigma_max of xform's 3x3 (largest axis scale).
 *   stretch    rigged only: max over the group mask's joints of jointStretch[skin][j]
 *              (sigma_max of that joint's current skin matrix relative to rest, >= 1; 2a fills
 *              the table, 1.0 until then) -- error measured in rest pose grows at most by it.
 *   anim_margin_m  conservative distance margin for animation displacement (0 in wave 1).
 * Per view (ClusterView), with c_w = xform * sphere.xyz, r_w = sphere.w * lod_scale,
 * err_w = error * lod_scale * stretch:
 *   perspective (main view, probe faces, snapshots, projector shadows):
 *       px = err_w * px_scale / max(|c_w - lod_cam| - r_w - anim_margin_m - lod_margin, znear)
 *       (lod_cam / lod_margin = ClusterView::lod_cam: the 2c controller's lagged LOD camera,
 *        |lod_cam - cam| <= lod_margin, so the error is never under-estimated; #749)
 *       px_scale = 0.5 * viewport_height_px * P[1][1]      (= px per metre at 1 m)
 *       (probe face 90 deg: P[1][1] = 1 -> px_scale = face_res / 2; snapshot: the snapshot's
 *        own height, so high-res photos get full detail)
 *   orthographic (sun cascades): px = err_w * px_scale, px_scale = viewport_px / ortho_extent_m
 *   tol = view.tol_px * instance.tol_scale (2c: protection = tol_scale < 1; RT views use
 *   RenderVKClusterRTTolerancePx).
 *
 * JOINT-STRETCH BOUND: ClusterRecord / ClusterGroup carry a 128-bit joint mask (joint
 * indices < 128, LLCleanQV::kJointMask) of every joint with nonzero weight in the cluster
 * (group: union over members and generating groups) and an anchor joint (largest summed
 * weight) for bounds transforms. Static meshes: mask 0 -> stretch 1.
 * ============================================================================================
 * ON-DISK FORMAT (1a, LLCleanClusterCache): one file per cluster mesh,
 *     <cache>/clusterlod/<mesh uuid>_f<face>_b<builder version>.clod
 * key = (mesh asset UUID, face index, kBuilderVersion). Layout:
 *     ClusterFileHeader | ClusterGroup[group_count] | ClusterPageDir[page_count] |
 *     uint32 deps[deps_count] | page payloads (page_dir[i].offset from payload_off)
 * Payload compression: kCompNone (raw used_bytes; always readable). kCompGDeflate is
 * reserved for the GPU-decompression upload tier (needs a vendored encoder; a reader that
 * cannot use it treats the file as a miss). Every read verifies magic, versions, header hash,
 * sizes and the page hash; any mismatch deletes the file (rebuilt on demand).
 */
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace LLClusterFmt
{
// ---- versions --------------------------------------------------------------------------
constexpr uint32_t kFormatVersion  = 3;   // GPU records + page layout (this header + the GLSL)
                                          // (2: ClusterView::lod_cam, #749; 3: ClusterMesh LOD0
                                          // triangle count in qheader[10], ClusterGroup flags
                                          // CLG_*, main-view stats counters)
constexpr uint32_t kFileVersion    = 1;   // .clod container
constexpr uint32_t kBuilderVersion = 3;   // 1a bumps on ANY change of its output (cache key; 3: root-page packing)
                                          // (2: the lower DAG floor -- released / tail levels)

// ---- cluster / group limits ------------------------------------------------------------
constexpr uint32_t kMaxClusterVerts   = 64;
constexpr uint32_t kMaxClusterTris    = 128;
constexpr uint32_t kMinClusterTris    = 16;    // builder hint (meshopt min_triangles, % 4 == 0)
constexpr uint32_t kGroupTarget       = 8;     // meshopt_partitionClusters target
constexpr uint32_t kMaxGroupClusters  = 32;    // hard cap (larger partitions are split)
constexpr uint32_t kMaxLevels         = 32;    // DAG depth cap (level fits 8 bits)

// ---- page ------------------------------------------------------------------------------
constexpr uint32_t kQVertexBytes      = 20;    // LLCleanQV::kStride
constexpr uint32_t kQWeightBytes      = 10;    // LLCleanQV::kWeightStride
constexpr uint32_t kQHeaderVerts      = 2;     // LLCleanQV::kHeaderVerts
constexpr uint32_t kPageBytes         = 81920;
constexpr uint32_t kPageWords         = kPageBytes / 4;               // 20480
constexpr uint32_t kPageVerts         = kPageBytes / kQVertexBytes;   // 4096
constexpr uint32_t kPageWeightRecs    = kPageBytes / kQWeightBytes;   // 8192
constexpr uint32_t kPageHeaderBytes   = 32;
constexpr uint32_t kClusterRecordBytes = 64;
constexpr uint32_t kMaxClustersPerPage = 64;
constexpr uint32_t kClusterIndexBits  = 6;     // cluster index within a page (resident ref)
constexpr uint32_t kPageMagic         = 0x47504C43u;   // 'CLPG'
// The pool is addressed with uint32 byte offsets (firstIndex of a uint8 index draw).
constexpr uint32_t kMaxPoolPages      = (uint32_t)(0xFFFFFFFFull / kPageBytes);   // 52428
// Global virtual page ids (page table entries): mesh.page_base + mesh-local page.
constexpr uint32_t kMaxVirtualPages   = 1u << 20;   // 4 MiB page table
constexpr uint32_t kMaxMeshes         = 1u << 16;   // mesh directory entries
constexpr uint32_t kMaxGroupsTotal    = 1u << 20;   // group table entries (48 B each)
constexpr uint32_t kMaxViews          = 16;         // cluster views per frame (main, 4 cascades,
                                                    // 6 probe faces, mirror, 4 spare)

// ---- sentinels -------------------------------------------------------------------------
constexpr uint32_t kNone      = 0xFFFFFFFFu;
constexpr uint32_t kGroupLeaf = 0;     // generating group of LOD0 clusters (error 0)
constexpr uint32_t kGroupRoot = 1;     // containing group of root clusters (error kErrorInf)
constexpr uint32_t kFirstRealGroup = 2;
constexpr float    kErrorInf  = 3.0e38f;   // finite (fast-math safe) "infinite" error

static_assert(kPageBytes % kQVertexBytes == 0 && kPageBytes % kQWeightBytes == 0 && kPageBytes % 16 == 0,
              "page base must be a multiple of the QVertex, QWeight and std430 vec4 strides");
static_assert(kMaxClustersPerPage <= (1u << kClusterIndexBits), "cluster index bits");
static_assert(kMaxClusterVerts <= 256, "uint8 local indices");
static_assert(kMaxClusterTris % 4 == 0 && kMaxClusterTris <= 512 && kMaxClusterVerts <= 256,
              "meshoptimizer meshlet limits");
static_assert((uint64_t)kMaxPoolPages * kPageBytes <= 0xFFFFFFFFull, "pool must stay < 4 GiB");

// ---- the cluster descriptor set (LLCleanClusterVK owns the layout; = CL_B_* in the GLSL) ----
enum ClusterBinding : uint32_t
{
    CLB_POOL = 0,        // uint[]            owner 1c   (page pool)
    CLB_PAGE_TABLE,      // uint[]            owner 1c
    CLB_MESHES,          // ClusterMesh[]     owner 1c
    CLB_GROUPS,          // ClusterGroup[]    owner 1c
    CLB_REQUESTS,        // header + reqs     owner 1c   (GPU-written by 1b)
    CLB_PAGE_USAGE,      // uint[]            owner 1c   (GPU-written by 1b)
    CLB_INSTANCES,       // ClusterInstance[] owner 1b
    CLB_VIEWS,           // ClusterView[]     owner 1b
    CLB_SELECTED,        // ClusterSelected[] owner 1b
    CLB_COUNTERS,        // uint[]            owner 1b
    CLB_DRAWS,           // uint[] (+INDIRECT)owner 1b
    CLB_STATS,           // uint[CLS_COUNT]   owner core (per frame slot)
    CLB_JOINT_STRETCH,   // header + float[]  owner 2a (core placeholder until then)
    CLB_HIZ,             // sampler2D         renderer import (core writes it)
    CLB_RESERVED_W1 = 14,   // 14..15: spare for wave 1 (1b / 1c)
    CLB_PICK = 14,          // 1b (#702 cluster pick): the GPU pick's cluster draw list   owner 1b
    CLB_INST_CUT = 15,      // 1b (#1011): per ClusterInstance, the main view's skinned cut summary
                            // {count, hash a, hash b, tris, frame + 1} (2b's derived RT cut)   owner 1b
    CLB_RESERVED_W2 = 16,   // 16..23: wave 2 (2a skinning, 2b RT, 2c controller)
    CLB_SKIN_OUT = 16,      // 2a (#747): skinned cluster arena     owner 2a (llcleanclusterskinvk.h)
    CLB_SKIN_SCRATCH = 17,  // 2a (#747): skin scratch (margins, map, weld) owner 2a
    CLB_COUNT = 24,         // every binding 0..23 exists in the layout (PARTIALLY_BOUND
                            // storage buffers unless bindingType() says otherwise), so
                            // a module claims a reserved number with setBuffer() alone
};
// Set index of the cluster set in cluster DRAW pipeline layouts ({mDslDraw0, mDslDraw1,
// cluster}); the module's compute pipelines use set 0 (GLSL: -DCLUSTER_SET=<n>).
constexpr uint32_t kClusterDrawSetIndex = 2;
constexpr uint32_t kClusterComputeSetIndex = 0;

// Cluster record flags (ClusterRecord::flags).
enum : uint32_t
{
    CLF_RIGGED      = 1u << 0,   // weight block present (weight_off10 valid)
    CLF_HAS_TANGENT = 1u << 1,   // at least one vertex carries a tangent
    CLF_DOUBLE_SIDED_HINT = 1u << 2,   // reserved (material decides; informational)
    CLF_BORDER_LOCKED = 1u << 3, // contains weld-boundary (#574) or layer-contact (#611) locks
};
// Group flags (ClusterGroup::flags; informational, kFormatVersion 3).
enum : uint32_t
{
    CLG_FACE_BORDER_FREE = 1u << 0,   // its simplification moved the face's open border (the seam to
                                      // the mesh's other faces): error >= 2 x sphere radius (G7)
};
// Mesh flags (ClusterMesh::flags / ClusterFileHeader::flags).
enum : uint32_t
{
    CLM_RIGGED = 1u << 0,
    CLM_STATIC = 1u << 1,        // phase 2 (dense static mesh)
};
// Page compression (ClusterFileHeader::compression).
enum : uint32_t
{
    kCompNone     = 0,
    kCompGDeflate = 1,           // GDeflate 1.0 streams (VK_EXT / NV_memory_decompression)
};

// ============================================================================================
// GPU RECORDS (std430; mirrored field by field in clean_cluster_common.glsl)
// ============================================================================================

// In-page header (page offset 0).
struct ClusterPageHeader
{
    uint32_t magic;          // kPageMagic
    uint32_t version_count;  // bits 0..7 cluster count, 8..15 group count, 16..31 kFormatVersion
    uint32_t mesh_key;       // low 32 bits of the mesh key hash (validation)
    uint32_t page_index;     // mesh-local page index
    uint32_t used_bytes;     // bytes of this page that carry data (<= kPageBytes)
    uint32_t group_first;    // first mesh-local group whose members are in this page
    uint32_t flags;          // reserved
    uint32_t hash;           // FNV-1a 32 of [32, used_bytes) (CPU check on load; GPU ignores)
};
static_assert(sizeof(ClusterPageHeader) == kPageHeaderBytes, "ClusterPageHeader");

// One cluster (64 B), at page offset kPageHeaderBytes + i * 64.
struct ClusterRecord
{
    float    sphere[4];      // tight bounds, MESH space: xyz centre, w radius (cull only)
    uint32_t joint_mask[4];  // joints with nonzero weight (bit j of word j/32); 0 = static
    uint32_t vert_off20;     // FIRST REAL vertex, page offset / 20 (QHeader at -2)
    uint32_t counts;         // bits 0..7 vert_count, 8..15 tri_count, 16..23 DAG level (0 = LOD0),
                             // 24..30 anchor joint, 31 unused
    uint32_t index_off4;     // index block page offset / 4
    uint32_t weight_off10;   // weight block page offset / 10 (kNone = no weights)
    uint32_t group;          // CONTAINING group (mesh-local; kGroupRoot for roots)
    uint32_t child_group;    // GENERATING group (mesh-local; kGroupLeaf for LOD0)
    uint32_t cone;           // backface cone in MESH space, snorm8: axis x | y << 8 | z << 16 | cutoff << 24
                             // (meshopt_Bounds cone_axis_s8 / cone_cutoff_s8; cutoff 127 = no cone)
    uint32_t flags;          // CLF_*
};
static_assert(sizeof(ClusterRecord) == kClusterRecordBytes, "ClusterRecord");

// One DAG group (48 B). Mesh-local index = position in the mesh's group table.
struct ClusterGroup
{
    float    sphere[4];      // LOD sphere, MESH space (G2: contains every generating group's)
    uint32_t joint_mask[4];  // union of members' + generating groups' masks (G3)
    float    error;          // MESH-space metres (G1: >= every generating group's)
    uint32_t page;           // mesh-local page holding ALL members (G4); kNone for sentinels
    uint32_t counts;         // bits 0..7 member cluster count, 8..15 level of its members,
                             // 16..22 anchor joint
    uint32_t flags;          // CLG_*
};
static_assert(sizeof(ClusterGroup) == 48, "ClusterGroup");

// One registered cluster mesh (= mesh asset + face), 96 B. Directory index = the mesh id
// every instance and request uses. Written by 1c at registration.
struct ClusterMesh
{
    float    qheader[12];    // [0..9] = LLCleanQV::QHeader (pos_c, pos_s, uv_c, uv_s), [10] = the
                             // face's LOD0 triangle count as uint bits (stats baseline,
                             // meshLod0Tris; kFormatVersion 3), [11] 0
    float    sphere[4];      // whole mesh, MESH space
    uint32_t page_base;      // its page 0's virtual page id (page table index)
    uint32_t page_count;
    uint32_t root_pages;     // pages [0, root_pages) pinned resident (G5)
    uint32_t group_base;     // its group 0 (kGroupLeaf) in the group table
    uint32_t group_count;    // including the two sentinels
    uint32_t cluster_count;
    uint32_t flags;          // CLM_*
    uint32_t generation;     // bumps when the directory slot is reused (stale-ref check)
};
static_assert(sizeof(ClusterMesh) == 96, "ClusterMesh");
inline void setMeshLod0Tris(ClusterMesh& m, uint32_t tris) { std::memcpy(&m.qheader[10], &tris, 4); }
inline uint32_t meshLod0Tris(const ClusterMesh& m) { uint32_t t; std::memcpy(&t, &m.qheader[10], 4); return t; }

// One cluster-drawn instance this frame (80 B), written by 1b's CPU gatherer.
struct ClusterInstance
{
    float    xform[12];      // mesh space -> world, 3x4 ROW-major (VkTransformMatrixKHR rows)
    uint32_t render_instance;// index into the renderer's Instance SSBO (material, bucket, flags)
    uint32_t mesh;           // ClusterMesh directory index
    uint32_t skin_id;        // skin palette / joint-stretch row (kNone = static)
    uint32_t flags;          // CLI_*
    float    tol_scale;      // per-instance tolerance factor (2c protection; 1 = normal)
    float    lod_scale;      // sigma_max of xform's 3x3
    float    anim_margin_m;  // animation distance margin (>= 0)
    uint32_t mesh_generation;// must equal ClusterMesh::generation (else the instance is skipped)
};
static_assert(sizeof(ClusterInstance) == 80, "ClusterInstance");
enum : uint32_t
{
    CLI_RIGGED     = 1u << 0,
    CLI_SELF       = 1u << 1,   // own avatar (self-hide in the main view, like selfHideW)
    CLI_NO_REQUEST = 1u << 2,   // never request pages for it (e.g. HUD / preview)
    CLI_DBG_LOCK_LEVEL = 1u << 3,  // debug: force the DAG level in flags bits 24..31
};

// One view (256 B). Up to kMaxViews per frame.
struct ClusterView
{
    float    view_proj[16];  // column-major, (world - origin) -> clip (as VKDrawPush)
    float    planes[6][4];   // world space, inside: dot(n, p) + d >= 0
    float    cam_pos[4];     // xyz world, w = znear (positive, metres)
    float    origin[4];      // xyz camera-relative origin of view_proj (VKDrawPush::origin)
    float    px_scale;       // perspective: px per metre at 1 m; ortho: px per metre
    float    tol_px;         // pixel tolerance of this view
    uint32_t flags;          // CLV_*
    uint32_t sel_base;       // first entry of this view's slice of the selected arena
    uint32_t sel_cap;        // its capacity (overflow is counted, never written past)
    uint32_t draw_base;      // first draw command of this view's slice (per bucket: 1b)
    uint32_t draw_cap;
    uint32_t hiz_size;       // Hi-Z mip 0 w | h << 16 (0 = no occlusion test)
    uint32_t frame;          // frame number (page usage stamps, request dedupe)
    uint32_t view_index;     // 0..kMaxViews-1
    float    viewport_px[2]; // render target size in pixels
    float    lod_cam[4];     // xyz: the LOD camera (#749, the error projection only; the cone
                             // test keeps cam_pos), w: its distance margin in metres (>= 0).
                             // The 2c controller lags it behind cam_pos for temporal stability
                             // and sets w >= |lod_cam - cam_pos|, so the projected error is never
                             // under-estimated. lod_cam = cam_pos, w = 0: the plain projection.
};
static_assert(sizeof(ClusterView) == 256, "ClusterView");
enum : uint32_t
{
    CLV_ORTHO      = 1u << 0,
    CLV_OCCLUSION  = 1u << 1,   // test against the Hi-Z (main view, after the pre-pass)
    CLV_REQUESTS   = 1u << 2,   // this view may request pages (main + snapshot; shadows /
                                // probes only consume what the main view made resident)
    CLV_RT         = 1u << 3,   // the RT cut (2b)
    CLV_SHADOW     = 1u << 4,
    CLV_PROBE      = 1u << 5,
    CLV_MAIN       = 1u << 6,
    CLV_NO_CONE    = 1u << 7,   // skip backface-cone culling (double-sided / shadows)
    CLV_HIDE_SELF  = 1u << 8,   // main view: our own avatar (CLI_SELF) is selected (the rig lists /
                                // RT keep its cut) but not drawn / picked (mouselook)
    CLV_NO_RIGGED  = 1u << 9,   // <GMDX> main view: the avatars render type is off -- no rigged instance
                                // is selected, so neither the rig lists nor the RT cut carry one
};

// Resident cluster reference: pool slot << kClusterIndexBits | index in page.
inline uint32_t makeClusterRef(uint32_t pool_slot, uint32_t idx) { return (pool_slot << kClusterIndexBits) | idx; }
inline uint32_t refSlot(uint32_t ref) { return ref >> kClusterIndexBits; }
inline uint32_t refIndex(uint32_t ref) { return ref & ((1u << kClusterIndexBits) - 1u); }

// One selected cluster instance (8 B), the output of selection for a view; consumed by the
// draw emission (1b), the cluster pre-skin (2a) and the RT cut (2b).
struct ClusterSelected
{
    uint32_t cluster_ref;    // makeClusterRef
    uint32_t instance;       // ClusterInstance index
};
static_assert(sizeof(ClusterSelected) == 8, "ClusterSelected");

// Page requests: header + entries (written by selection shaders, read back by 1c).
struct ClusterRequestHeader
{
    uint32_t count;          // entries appended (may exceed cap: the excess is dropped)
    uint32_t cap;            // capacity in entries (1c writes it)
    uint32_t frame;          // frame that wrote it
    uint32_t pad;
};
struct ClusterRequest
{
    uint32_t vpage;          // virtual page id (mesh.page_base + local page)
    float    priority;       // projected error of the group that wants it, in px (larger first)
};
static_assert(sizeof(ClusterRequestHeader) == 16 && sizeof(ClusterRequest) == 8, "requests");

// GPU statistics counters (uint32 each), one block per frame slot.
enum : uint32_t
{
    CLS_INSTANCES = 0,       // instances processed
    CLS_TESTED,              // resident clusters tested
    CLS_CUT,                 // clusters passing the cut rule
    CLS_FALLBACK,            // ... of which drawn because a finer page was missing
    CLS_CULL_FRUSTUM,
    CLS_CULL_OCCLUSION,
    CLS_CULL_CONE,
    CLS_SELECTED,            // written to the selected list (all views)
    CLS_SEL_OVERFLOW,        // dropped: a view's slice was full
    CLS_TRIANGLES,           // triangles selected (all views)
    CLS_VERTICES,
    CLS_REQUESTS,            // requests appended (incl. dropped)
    CLS_REQ_OVERFLOW,
    CLS_DRAWS,               // draw commands / mesh tasks emitted
    // main view only (CLV_MAIN; kFormatVersion 3) -- the 10 s CLOD summary's LOD baseline:
    CLS_MAIN_INST,           // instances (faces) with at least one cluster selected
    CLS_MAIN_ROOT,           // ... of which the whole cut is root clusters (the DAG floor)
    CLS_MAIN_CUT_TRIS,       // triangles of their cut (before frustum / cone / occlusion culling)
    CLS_MAIN_FULL_TRIS,      // their LOD0 triangles (meshLod0Tris): the full-detail baseline
    CLS_MAIN_SEL_TRIS,       // triangles selected in the main view (after culling)
    CLS_COUNT = 32
};

// ============================================================================================
// PAGE LAYOUT HELPERS (builder + validation)
// ============================================================================================
inline uint32_t alignUp(uint32_t v, uint32_t a) { return (v + a - 1u) / a * a; }
inline uint32_t clusterVertexBlockBytes(uint32_t vert_count) { return (vert_count + kQHeaderVerts) * kQVertexBytes; }
inline uint32_t clusterWeightBlockBytes(uint32_t vert_count) { return vert_count * kQWeightBytes; }
inline uint32_t clusterIndexBlockBytes(uint32_t tri_count) { return alignUp(tri_count * 3u, 4u); }

// Bytes a page needs for these clusters (vertex / triangle counts per cluster) in the
// layout above. The builder adds whole groups while this stays <= kPageBytes.
inline uint32_t pageBytesFor(const uint32_t* vert_counts, const uint32_t* tri_counts, uint32_t n, bool rigged)
{
    uint32_t off = alignUp(kPageHeaderBytes + n * kClusterRecordBytes, kQVertexBytes);
    for (uint32_t i = 0; i < n; ++i) off += clusterVertexBlockBytes(vert_counts[i]);
    if (rigged)
        for (uint32_t i = 0; i < n; ++i) off += clusterWeightBlockBytes(vert_counts[i]);
    off = alignUp(off, 4u);
    for (uint32_t i = 0; i < n; ++i) off += clusterIndexBlockBytes(tri_counts[i]);
    return off;
}
inline bool packPageFits(const uint32_t* vert_counts, const uint32_t* tri_counts, uint32_t n, bool rigged)
{
    return n <= kMaxClustersPerPage && pageBytesFor(vert_counts, tri_counts, n, rigged) <= kPageBytes;
}
// A full group of full clusters always fits in one page (G4 is always achievable).
static_assert(kPageHeaderBytes + kMaxGroupClusters * kClusterRecordBytes
              + kMaxGroupClusters * ((kMaxClusterVerts + kQHeaderVerts) * kQVertexBytes
                                     + kMaxClusterVerts * kQWeightBytes + kMaxClusterTris * 3u)
              + kQVertexBytes + 4u <= kPageBytes, "a maximal group must fit one page");

// Unpacking (CPU mirrors of the GLSL helpers).
inline uint32_t recVertCount(const ClusterRecord& r) { return r.counts & 0xFFu; }
inline uint32_t recTriCount(const ClusterRecord& r) { return (r.counts >> 8) & 0xFFu; }
inline uint32_t recLevel(const ClusterRecord& r) { return (r.counts >> 16) & 0xFFu; }
inline uint32_t recAnchorJoint(const ClusterRecord& r) { return (r.counts >> 24) & 0x7Fu; }
inline uint32_t packRecCounts(uint32_t verts, uint32_t tris, uint32_t level, uint32_t anchor)
{
    return (verts & 0xFFu) | ((tris & 0xFFu) << 8) | ((level & 0xFFu) << 16) | ((anchor & 0x7Fu) << 24);
}

// ============================================================================================
// CUT RULE + PROJECTION (CPU reference; clean_cluster_common.glsl is the GPU twin -- keep the
// arithmetic identical: the decision must match on both sides of every group boundary)
// ============================================================================================
// World-space sphere of a mesh-space sphere under a 3x4 row-major xform.
inline void xformSphere(const float x[12], float lod_scale, const float s[4], float out[4])
{
    for (int r = 0; r < 3; ++r) out[r] = x[r * 4 + 0] * s[0] + x[r * 4 + 1] * s[1] + x[r * 4 + 2] * s[2] + x[r * 4 + 3];
    out[3] = s[3] * lod_scale;
}
// Projected error in pixels of a GROUP for one instance in one view.
inline float projectGroupError(const ClusterGroup& g, const ClusterInstance& inst, const ClusterView& v, float stretch)
{
    if (g.error >= kErrorInf) return kErrorInf;
    if (g.error <= 0.f) return 0.f;
    float c[4];
    xformSphere(inst.xform, inst.lod_scale, g.sphere, c);
    const float err_w = g.error * inst.lod_scale * stretch;
    if (v.flags & CLV_ORTHO) return err_w * v.px_scale;
    const float dx = c[0] - v.lod_cam[0], dy = c[1] - v.lod_cam[1], dz = c[2] - v.lod_cam[2];
    const float d = std::sqrt(dx * dx + dy * dy + dz * dz) - c[3] - inst.anim_margin_m - v.lod_cam[3];
    return err_w * v.px_scale / (d > v.cam_pos[3] ? d : v.cam_pos[3]);
}
// The cut rule. parent_px = proj(group(C)), self_px = proj(child_group(C)),
// child_resident = the page of child_group(C) is resident (true for kGroupLeaf).
inline bool clusterRenders(float parent_px, float self_px, bool child_resident, float tol)
{
    return parent_px > tol && (self_px <= tol || !child_resident);
}

// ============================================================================================
// ON-DISK FORMAT
// ============================================================================================
constexpr uint32_t kFileMagic = 0x444F4C43u;   // 'CLOD'

struct ClusterFileHeader
{
    uint32_t magic;            // kFileMagic
    uint32_t file_version;     // kFileVersion
    uint32_t format_version;   // kFormatVersion
    uint32_t builder_version;  // kBuilderVersion (also in the file name)
    uint32_t header_bytes;     // sizeof(ClusterFileHeader)
    uint8_t  mesh_id[16];      // mesh asset UUID
    int32_t  face;             // face index
    int32_t  source_lod;       // creator LOD the DAG was built from (3 = high)
    uint32_t flags;            // CLM_*
    uint64_t source_hash;      // 64-bit hash of the source geometry (vertices, indices, weights)
    uint32_t page_bytes;       // kPageBytes
    uint32_t page_count;
    uint32_t root_pages;
    uint32_t group_count;      // incl. the two sentinels
    uint32_t cluster_count;
    uint32_t level_count;
    uint32_t tri_count_lod0;   // triangles of the source (stats / budget estimates)
    uint32_t compression;      // kComp*
    ClusterMesh mesh;          // qheader + sphere (+ counts); runtime fields (page_base,
                               // group_base, generation) are 0 on disk
    uint32_t groups_off;       // byte offsets from the file start
    uint32_t pagedir_off;
    uint32_t deps_off;
    uint32_t deps_count;
    uint32_t payload_off;
    uint32_t pad0;
    uint64_t payload_bytes;
    uint32_t header_hash;      // FNV-1a 32 of this struct with header_hash = 0
    uint32_t pad1;
};
static_assert(sizeof(ClusterFileHeader) == 224, "ClusterFileHeader");
static_assert(offsetof(ClusterFileHeader, mesh) == 88, "ClusterFileHeader layout (the mesh record is memcpy-ed, not aliased)");

struct ClusterPageDir
{
    uint32_t offset;           // from payload_off
    uint32_t stored_bytes;     // bytes on disk (== used_bytes when kCompNone)
    uint32_t used_bytes;       // uncompressed page bytes (<= kPageBytes)
    uint32_t hash;             // FNV-1a 32 of the stored bytes
    uint32_t dep_first;        // first parent page in deps[] (G6)
    uint16_t dep_count;
    uint16_t group_count;      // groups whose members are in this page
    uint32_t group_first;      // first of them (mesh-local)
    uint32_t level;            // coarsest DAG level in the page (coarse -> fine order, G5)
};
static_assert(sizeof(ClusterPageDir) == 32, "ClusterPageDir");

// FNV-1a 32 (page / header hashes).
inline uint32_t fnv1a32(const void* data, size_t n, uint32_t h = 2166136261u)
{
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 16777619u; }
    return h;
}
} // namespace LLClusterFmt
