# Cluster LOD: the shared contract (#642, wave 0 = #743)

Read this before touching any cluster LOD file. It covers:

- what the shared format is;
- who owns which file and which GPU buffer;
- the interfaces between the modules;
- the order of operations in a frame.

The code is the authority. `llcleanclusterformat.h` holds the C++ structs and static_asserts;
`indra/vkclean/shaders/clean_cluster_common.glsl` is its GLSL twin. This document explains them.

**Method rule.** Implement only from published material: papers, talks (the SIGGRAPH 2021
Nanite talk), and meshoptimizer's MIT code (`demo/nanite.cpp`, credited in the file headers).
Never copy engine source. Never cite other viewers' code in commits.

## 1. Owner rules (non-negotiable)

- **Toggle.** `RenderVKClusterLOD` defaults to off.
  - Off: the module is never created, and every hook is a no-op behind `mCluster` /
    `clusterLodOn()`. The rigged LOD controller (#561 / #613) and every current path are
    unchanged. The old controller stays, possibly permanently, as the RT path for
    first-generation RT cards.
  - Turned off at runtime: the module winds down (`idleRelease`) and is deleted after
    `kFrameSlots` frames.
- **The cluster path REPLACES the legacy one (2026-10-14, owner decision).** Legacy is a separate
  WHOLE-SESSION mode (`RenderVKClusterLOD` off), for GPUs that run clusters worse. With it on there
  is no per-object fallback, not even transitional:
  - **Cluster-owns-rigged mode** (`LLCleanRendererVK::clusterOwnsRigged`, evaluated in
    `clusterTick`, logged on every flip): LOD on, the module and 2a working,
    `RenderVKClusterRigged` != 0, `RenderVKDeferredLighting` on and the frames actually lit
    deferred or forward WITH the `CLPASS_FORWARD` pipelines (120 consecutive forward-lit on-screen
    frames without them hand rigged meshes back to legacy; a shorter such run draws no cluster-path
    mesh). Forward-lit frames -- supersampled snapshots with RT shadows (their RT mask is off, so
    `deferredWanted` refuses), `RenderVKSnapshotDeferred` off, a pass 1 that is not split, debug
    views -- draw the main view's clusters in the forward lit pass 1 (`CLPASS_FORWARD`, plain or
    ray-query lit FS like the captures; 2026-10-14: before it, snapshots lost every cluster-owned
    rigged mesh). Every cluster-ELIGIBLE rigged row
    (`LLCleanClusterSelectVK::rowClusterRoutable`, row content only: a skinned mesh row that is
    not HUD / worn rigid / invisiprim / flexi / system body / bounds-unknown; ALPHA-BLEND rows are
    eligible since 2026-10-14 -- drawn from the ordered alpha lists, §9 "Rigged alpha-blend")
    belongs to the cluster path from its first frame, drawable or not.
  - **No legacy GPU copy**: such a row's LOD group gets no legacy geometry (`fillFromCleanStores`
    "CLUSTER-ROUTED GEOMETRY": never placed in the packed VBO / IBO / weight arena; placed slots
    are dropped when a group becomes routed-only; a mode flip repacks -- on: the buffers shrink to
    1.5x the need, off: the rigged slots stream back from the mirror, one hitch). The `.clod`
    cache stays; the mesh store's CPU mirror (the builder's source) is FREED once the face's DAG
    is durable (below). No legacy draw in any pass, no
    legacy pre-skin / weld / rig copy / RT BLAS; legacy rigged buffers are sized from what really
    stays legacy.
  - **CPU mirror release** (2026-10-16): a Live mesh whose DAG is from creator LOD 3 and on disk
    (`pageSource` Ready / Full: the write landed or the metadata came from the file) is offered by
    1c (`takeMirrorCandidates`, every 600 frames); the renderer (`clusterMirrorTick`) queues every
    level slot of its LOD group when the group is routed-only and the slot is unplaced, and the
    store frees the mirrors at the ingest sync point (`LLCleanMeshAsset::pumpMirrorReleases`;
    Slot, SlotInfo, publication, latch and skin info stay). Not durable (lower-LOD build, write
    refused / failed, read-only or disabled cache) = kept. Needed again -> `requestMirror`: the
    packer's placement of a legacy-needed slot (mode flip off, a legacy row on the group) and the
    builder's `request` / admission (cache miss, corrupt / evicted `.clod`, builder version,
    rebuild) ask; the ingest wakes the group's objects and `registerCleanFace` re-decodes that
    LOD (retained raw bytes, else LL's repository again) and re-captures it in place, async.
    Until then the slot has no legacy geometry / build source (logged). `RenderVKClusterFreeMirrors`
    0 keeps new mirrors (the weld probe skips freed ones). Stats: the 5 s VKMem `meshstore` segment
    (`mirror freed N slots (X MB), rematerialize pending P`).
  - **Per mesh, pop-in**: a mesh draws from clusters as soon as its root pages are resident;
    before that it is NOT DRAWN (the avatar loading cloud covers it). An avatar can be partly
    drawn while its meshes arrive.
  - **Statics** keep the old rule (`RenderVKClusterStatic 2`: owned only while drawn, legacy
    otherwise): probe faces and projector tiles have no cluster static draw, and cluster RT admits
    only the important statics (the rest keep their legacy BLAS) -- removing their legacy
    geometry would make them vanish there. The emissive light build (#1009) also lists a
    cluster-traced static's triangles from its legacy slot (`clean_rt_instances.comp`
    `clusterStaticEmRow` -> `rt_geom_vc` words 1..3), so its glowing faces keep lighting the scene.
- **Newest feature first.** Each capability picks its tier from the extensions and features
  present at runtime, never from the GPU name. Slower tiers are additive fallbacks.
  `RenderVKClusterForceTier` caps every capability, so each fallback can be tested on the RTX 5090.
- **Never crash.**
  - A missing page draws the coarser parent: the cut rule does this by itself.
  - A failed allocation falls back the same way. Allocations are soft (`SoftAllocScope`); a
    failure is retried later or the item is dropped and logged.
  - A failed init leaves the legacy path drawing and retries 600 frames later.
- **No unbounded growth.** Pools are fixed and grow only in steps. Everything has a byte budget
  and reports live / cap / peak. Meshes are freed on liveness after `RenderVKClusterMeshGraceSec`.

## 2. File ownership

Each agent edits only its own files, plus the lines in shared files that carry its own
`TODO(<id>)` marker. If you need a change in someone else's file, write it up in your report;
don't make it.

| Agent | Owns | Shared-file edits it may make |
|---|---|---|
| **0 (core, done)** | `llcleanclusterformat.h`, `clean_cluster_common.glsl`, `vkgfx/llcleanclustervk.{h,cpp}`, `vkgfx/llcleanclustercaps.{h,cpp}`, this document, the cluster block in `vk_device.cpp`, the hooks in the renderer | none |
| **1a ingest** (#744) | `llcleanclusterbuild.{h,cpp}`, `llcleanclustercache.{h,cpp}` | cache init/shutdown in `llappviewer.cpp`, next to the GPU texture cache (marked by you) |
| **1b selection / emission** (#745) | `vkgfx/llcleanclusterselectvk.{h,cpp}`; new shaders `clean_cluster_select*.comp`, `clean_cluster_draw*.{vert,task,mesh}`, a `-DCLEAN_CLUSTER` variant of `clean_draw.vert`, and their `SHADERS` rows in `regen_world_spv.py` | renderer `TODO(1b)` lines (legacy exclusion in `dispatchCull` / `clean_cull.comp`, Hi-Z import in `clusterTick`); mesh/task stages in `vk_pipeline.{h,cpp}` (`GraphicsPipelineDesc`) |
| **1c streaming** (#746) | `vkgfx/llcleanclusterstreamvk.{h,cpp}`; any `clean_cluster_stream*.comp` it adds | renderer `TODO(1c)` (async compute queue at `clusterTick`); `LLCleanClusterVK::idleRelease` (`TODO(1c)`) |
| **2a skinning** (#747, done) | `vkgfx/llcleanclusterskinvk.*`, `clean_cluster_preskin*.comp`, `clean_cluster_skin_common.glsl` | the `CLB_JOINT_STRETCH` buffer (replacing the core placeholder through `setBuffer`), bindings 16 / 17; 1b's gather hooks, `SelectPush.rig_bounds`, the skinned read in `clean_cluster_draw_common.glsl`; `RC_CLUSTER` in `clean_cull.comp` / `clean_draw_rigged.vert`; the renderer's skin inputs (`clusterTick`) and cluster pre-skin call |
| **2b RT** (#748, done) | `vkgfx/llcleanclusterrtvk.*` (owned by `LLCleanRTVK`), `clean_cluster_rt.comp` (full + `-DCL_RT_PTLAS_ONLY`) | `llcleanrtvk.*` call sites (`TODO(2b)` markers); `clean_rt_instances.comp` bindings 22 / 23 + mode 4 + pass B's switched groups; the hit funnel in `clean_rt_common.glsl` (+ RT set binding 13) + `RT_HIT_PRIM` at the hit sites (`clean_shadow_rt.glsl`, `clean_rt_shadow_mask.comp`, `clean_probe_rt.comp`); the rigged plan's switched-group key in `llcleanrtvk.cpp` (rigged cluster RT, #747 + #748) |
| **2c controller** (#749) | new `vkgfx/llcleanclustercontrolvk.*` | `clusterCullMain` tolerance / znear `TODO(2c)`; more `clusterCull*` calls at `recordShadows` / `recordProbeFace` / `recordMirror`; the `tol_scale` line in 1b's gatherer |

Descriptor bindings 0..23 all exist in the layout already. 14–15 are spare for wave 1 and 16–23
are for wave 2; they are placeholder storage buffers, `PARTIALLY_BOUND`.
- To claim one: name it in `ClusterBinding` / `CL_B_*` and call `setBuffer()`.
- If the binding is not a storage buffer, also add its type to `bindingType()` in
  `llcleanclustervk.cpp`.
That is the only edit other modules make in core files.

## 3. Data format (see `llcleanclusterformat.h` for every field)

**Cluster**
- At most 64 vertices and 128 triangles. `kMinClusterTris` = 16 is a builder hint.
- Why these sizes:
  - **Mesh shader:** a 64-invocation workgroup per cluster gives one vertex and two triangles
    per invocation. The RTX 5090's maxMeshOutputMemorySize is 32 KiB; 64 vertices of G-buffer
    varyings use about 8 KiB, while 128 would crowd the limit.
  - **NV cluster acceleration structures:** the limits are queried at runtime. The tier is
    refused unless they are at least 64 vertices / 128 triangles (RTX 50: 256 / 256).
  - **uint8 indices** need at most 256 vertices.
  - **Pre-skin:** one 64-thread workgroup per cluster.
  - **meshoptimizer:** its limits are 256 vertices / 512 triangles, triangles divisible by 4.

**Index format.** uint8 local indices, 3 per triangle, packed into 4-byte-aligned blocks. The
RTX 5090 reports `indexTypeUint8 = true`.
- Mesh tier: the indices are read as words.
- `MDI_U8` tier: the pool is bound as a `VK_INDEX_TYPE_UINT8` index buffer.
- Floor tier: the vertex shader pulls the index from the pool by `gl_VertexIndex`.
- Consequence: the pool must stay under 4 GiB (`kMaxPoolPages` = 52428).

**Page.** `kPageBytes` = 81920 (80 KiB). That is 4096 QVertex (20 B) = 8192 QWeight (10 B) = 5120 × 16 B.
- The pool is one buffer of N pages. Because every page base is a multiple of 20, 10 and 16,
  the existing consumers address page data in their own units:
  - vertex = `slot*4096 + vert_off20`
  - weight = `slot*8192 + weight_off10`
  - index byte = `slot*81920 + index_off4*4`
- Layout inside a page:
  1. header (32 B)
  2. cluster records (64 B each)
  3. vertex blocks: QHeader + verts, each block starting on a multiple of 20
  4. weight blocks: rigged only, 10 B per vertex
  5. index blocks
- **Pages hold whole groups (G4).** A maximal group (32 full clusters) always fits; this is
  static_asserted.
- **The pool is format-compatible with the mega-VBO, not the same buffer.** Consumers bind the
  pool where they bind the mega-VBO today:
  - the draw vertex shader's header words come from `CL_B_POOL` (a `-DCLEAN_CLUSTER` variant);
  - a cluster pre-skin binds the pool at bindings 0 and 1;
  - the weld reads the same.

**Quantization lattice.** There is one QHeader per cluster mesh (mesh asset + face), built from
LOD0's extent and copied into every cluster block at every level. Simplified vertices are
re-encoded on it, so locked vertices coincide bit-for-bit and the result is watertight.
`ClusterMesh::qheader[10]` (after the 10-float QHeader) carries the face's LOD0 triangle
count as uint bits (`meshLod0Tris` / `clMeshLod0Tris`, `kFormatVersion` 3). It is the stats
baseline, and 1c sets it at registration.

**Weights.** Weights use `LLCleanQV::QWeight` unchanged. The weld bit (bit 7 of `j[3]`) means
"open boundary of the ORIGINAL face". 1a sets it by position match against LOD0's
open-boundary set; it does NOT run `markWeldBoundary` per cluster.

**Clusters never cross faces.** A cluster mesh is one face, so it has one material.

## 4. DAG, cut rule, residency

- `group(C)` is the CONTAINING group: the one whose simplification made C's parents.
- `child_group(C)` is the GENERATING group: the one whose simplification made C.
- Every mesh's group table starts with two sentinels:
  - `kGroupLeaf` = 0: error 0. It is the generating group of LOD0 clusters.
  - `kGroupRoot` = 1: error `kErrorInf`. It is the containing group of the roots.

Builder guarantees, which `LLCleanClusterBuilder::validate` checks:

| | Guarantee |
|---|---|
| G1 | Errors are monotonic: `error(G) = max(simplify error, generating groups' errors)`. |
| G2 | Group spheres are nested. |
| G3 | Joint masks are nested (union). |
| G4 | One page per group. |
| G5 | Pages run coarse → fine; root pages come first and are pinned. Builder v3 (2026-10-14): the LAST root page also holds the coarsest groups that fit (pinned with it; its dependencies are root pages). |
| G6 | Each page has a parent-page dependency list. |
| G7 | The face's open border is locked at every level except in `CLG_FACE_BORDER_FREE` groups, whose error is at least their sphere's diameter. |

**The DAG floor (builder version 2).** Each face's DAG continues past the point where the
locked simplification stalls, in three stages:
1. **Strict.** Group borders and the face's open border (the seam to the mesh's other faces,
   the #574 weld set) are locked, and meshopt's error is the level's error. A group stuck on
   attribute seams retries with `meshopt_SimplifyPermissive`; the attribute metric still
   prices it. A lone frontier cluster keeps simplifying too.
2. **Released.** Starts once strict stalls. Group borders stay locked, so a cut is always
   watertight inside the face. The face's open border is free. The group's error is raised to
   at least its sphere's diameter. Every output vertex is a LOD0 vertex inside that sphere,
   and so is all the original geometry below the group. That bounds the deviation, and with it
   any crack to a neighbouring face. Such a level is only cut in where the whole patch
   projects within the tolerance.
3. **Final.** The whole remaining face becomes one unlocked group: at most `kGroupTarget`
   clusters, or a stuck released frontier of at most `kMaxGroupClusters`. It goes straight to
   about 4 triangles, through prune and then `meshopt_simplifySloppy` when the regular
   simplifier stops short. Its error is at least the face's diameter.

4. **Prune** (2026-10-13, still builder version 2). When the released stage stalls with more than
   `kMaxGroupClusters` clusters (too many for the final group: many small disconnected parts such as
   hair strands, lashes or cards, which the topology-preserving simplifier cannot shrink), the
   following released levels prune: a stuck group drops whole connected parts that touch no locked
   (group-border) vertex, smallest extent first, then simplifies what is left. No group border
   moves, so there is no crack inside the face; the error is at least the sphere's diameter, as for
   any released group. Before, such a face ended there: `no DAG convergence` past `kMaxRootPages`
   (the cap was never the problem: 440 root clusters on a 14.6k-triangle face meant nothing had
   been simplified), or a root of hundreds of clusters. `kBuilderVersion` was not bumped: only
   faces that reached this stall build differently, and their old cached files stay valid DAGs.

A face's floor is therefore a handful of triangles, reached only where the whole face is
within the tolerance. Released border vertices have no weld partner, which is fine within
the error. Considered and not done:
- **Faces sharing coarse levels:** clusters never cross faces (one material, one cache key per
  face).
- **An empty root:** the RT tier-0 per-cut BLAS and the streamer's pinned roots assume at
  least one root cluster, and a handful of triangles costs as much as nothing.

`validateCuts` (debug) also samples the cut where each border-free group first appears, and
two far views. In a cut holding border-free clusters, every open edge must run between LOD0
border positions; a crack along a group border would expose interior positions. A cut of one
generating group (the final tail) is a single simplified surface.

**Cut rule** (`clCutRenders` / `LLClusterFmt::clusterRenders`):

```
render(C) = resident(page(C))
          ∧ proj(group(C)) > tol
          ∧ (proj(child_group(C)) ≤ tol ∨ ¬resident(page(child_group(C))))
```

- `proj()` reads only a group record (sphere, error, mask), through one function. Both sides of
  every boundary therefore make the identical decision, which keeps the result watertight.
- G1–G3 make `proj()` monotonic up the DAG, so exactly one cut exists.
- A missing finer page makes the parent render (the fallback) and adds a request.

**Residency closure** (1c must enforce it):
- A page becomes resident only after all its parent pages are.
- A page is evicted only after every resident page that depends on it.
- Without closure a child group and its parents' parents could both render, which overlaps.

**Table updates.** A page-table entry flips to resident after its copy, with a barrier on the
same queue. It flips to non-resident before its slot is reused; the slot is overwritten only
`kFrameSlots` frames later.

## 5. Error units and projection

- **Units:** metres in the mesh space of the decoded QVertex positions. Bounds and errors use
  the same space.
- **Per instance** (`ClusterInstance`, written by 1b):
  - `xform`: 3×4, mesh → world, rest pose. For rigged meshes this is the avatar world transform
    times the bind shape.
  - `lod_scale` = σ_max of `xform`.
  - `tol_scale`: 2c protection.
  - `anim_margin_m`.
  - `stretch` = max of `jointStretch[skin][j]` over the group mask (≥ 1, from 2a; 1 until then).
- **Per view** (`ClusterView`):
  - **Perspective:** `px = err·lod_scale·stretch · px_scale / max(|c−lod_cam| − r − margin − lod_margin, znear)`,
    with `px_scale = 0.5·H·P[1][1]`. `lod_cam` / `lod_margin` = `ClusterView::lod_cam` (#749,
    `kFormatVersion` 2): the controller's lagged LOD camera, `|cam − lod_cam| ≤ lod_margin`, so
    the error is never under-estimated. The frustum, cone and occlusion tests keep `cam_pos`.
    - Probe face: `face_res/2`.
    - Snapshot: the snapshot height, so photos get full detail.
  - **Orthographic** (cascades): `px = err·lod_scale·stretch · px_per_metre`.
  - **Tolerance:** `tol = view.tol_px · inst.tol_scale`. RT views use `RenderVKClusterRTTolerancePx`.
- **Watertightness:** one xform per instance for every group, so nesting survives the transform.
  2a may refine the distance (per-joint anchors) only with a function that stays monotonic.

## 6. On-disk format

- **File:** `<cache>/clusterlod/<mesh uuid>_f<face>_b<kBuilderVersion>.clod`. The key is mesh
  asset + face + builder version (3 since 2026-10-14: root-page packing), and 1a bumps `kBuilderVersion` on any change to its output
  (2: the DAG floor, §4).
- **Layout:**
  1. `ClusterFileHeader` (224 B)
  2. `ClusterGroup[]`
  3. `ClusterPageDir[]` (32 B each)
  4. `uint32 deps[]`
  5. page payloads
- **Compression:**
  - `kCompNone` is always readable.
  - `kCompGDeflate` is reserved for the GPU-decompression tier. It needs a vendored GDeflate
    *encoder* (an MIT one exists publicly; vendoring it is 1c's decision). A reader that cannot
    use it treats the file as a miss.
- Every read is verified. A bad file is deleted and rebuilt.
- The cache follows the #274 pattern: `.part` + rename, LRU by mtime under `RenderVKClusterCacheMB`.

## 7. Tiers (`vkgfx/llcleanclustercaps.h`)

| Capability | 0 (floor) | 1 | 2 |
|---|---|---|---|
| RASTER | MDI + vertex pulling (core) | MDI, uint8 indexed (`index_type_uint8`) | task + mesh (`EXT_mesh_shader`, 1.3 path) |
| CMDGEN | `drawIndirectCount` | `EXT_device_generated_commands` (+ maintenance5) | – |
| UPLOAD | staging + `vkCmdCopyBuffer` | `KHR` / `NV_copy_memory_indirect` | `EXT` / `NV_memory_decompression` (GDeflate) — HELD at 1 |
| RT (−1 = RT off) | per-cut BLAS | `NV_cluster_acceleration_structure` | + `NV_partitioned_acceleration_structure` |
| ATOMIC64 | 32-bit | `shaderBufferInt64Atomics` (+ image int64) | – |

- **Which extensions `vk_device.cpp` enables** (the "CLUSTER LOD" block there has the audit):
  - With `RenderVKClusterLOD` on at startup (`setClusterRequested`): every one the device has.
  - Off at startup: the ones that are inert while unused, so cluster LOD switched on
    mid-session runs their tiers without a restart. `RenderVKClusterDeviceFeatures` chooses:
    - 0 = none: the device exactly as before #743.
    - 1 (default) = the inert ones: uint8 indices, copy-indirect, memory decompression, CLAS,
      shaderInt64 + 64-bit buffer atomics, image int64 atomics.
    - 2 = also mesh shaders. The extension itself is inert, but the renderer then adds
      TASK | MESH to the vertex-visible bindings of its draw set 0, so the cluster mesh
      pipelines can share the set. That is a descriptor-layout change, so it is opt-in.
  - Startup-only, because they are not inert:
    - DGC + `VK_KHR_maintenance5`: maintenance5 makes `vkGetDeviceProcAddr` return NULL for
      names of extensions that are not enabled. DGC is not used yet (CMDGEN tier 1 is a TODO).
    - Partitioned AS: `LLCleanRTVK` makes the session's TLAS partitioned when the device has
      it and `RenderVKClusterRT` ≥ 2.
    - The tier log line says "needs RenderVKClusterLOD on at startup (restart)" for each.
- They record nothing unless the cluster path runs. CLAS / PTLAS additionally need RT on.
- UPLOAD tier 2 is never native yet: with GDeflate decompression present, `detect()` holds the
  tier at 1 and says so in the line's `held lower:` part. No GDeflate encoder is in the tree,
  so the cache holds uncompressed pages; the tier line reports the path the stream uses. The
  stream's `statsLine` tier (`copy` / `indirect`) is the per-queue fact (a family without
  indirect copies falls back to staging copies).
- If `vkCreateDevice` refuses them, it is retried without them (`clusterFeatures().retried`).
- **When maintenance5 is enabled** (with `RenderVKClusterLOD` at startup on GPUs with DGC): the
  proc-address rule was audited. Every device-level fetch in the tree (`vk_compat` 1.2-path
  KHR names, breadcrumbs, fault, VRS, RT / OMM, VMA at the created API version) names an
  enabled extension or a core function. New code must keep that true.
- At startup one line is logged: `Cluster LOD tiers (RenderVKClusterForceTier=N): raster=… | …`,
  with the device mode (`device: every cluster extension …` or `device:
  RenderVKClusterDeviceFeatures=N …`) and the notes above. The module logs the line again on
  every live change of `RenderVKClusterForceTier`.
- Modules must build pipelines for every tier the native caps allow, and pick per frame by
  `caps().get(cap)`, so a `ForceTier` change takes effect the next frame:
  - raster: `drawTier()`;
  - upload: `mUploadTier`, with the staging / pool usage made for the native tier;
  - RT: `plan()`.

## 8. The cluster descriptor set

`LLCleanClusterVK` owns the layout. All bindings are `PARTIALLY_BOUND`, and there is one set per
frame slot.
- **Set index:** 0 in the module's compute pipelines; `kClusterDrawSetIndex` = 2 in draw
  pipelines (`{mDslDraw0, mDslDraw1, cluster}`).
- **Push range:** a draw layout keeps the renderer's push range `{VS|FS, 0, 16}` so sets 0/1 and
  the pushed view survive the bind. A mesh layout re-binds them from `LLClusterDrawIn`.
- **Writing descriptors:** owners never write descriptors. They call `setBuffer(binding, buf,
  offset, range, slot)`, and the set of each slot is rewritten when that slot comes round.
- **Freeing buffers:** owners free old buffers with `retire()`, which frees them after `kFrameSlots` frames.

| # | Name | Type / usage | Owner (allocates) | GPU writers | Readers | VKMEM |
|---|---|---|---|---|---|---|
| 0 | `POOL` | SSBO; STORAGE \| TRANSFER_DST \| VERTEX \| INDEX (+ DEVICE_ADDRESS for indirect-copy / decompress tiers, + AS_BUILD_INPUT when RT ≥ 0) | 1c | 1c (transfer / decompress) | 1b selection, draw VS / mesh, 2a pre-skin, 2b CLAS build | GEOMETRY |
| 1 | `PAGE_TABLE` | SSBO uint[kMaxVirtualPages]; STORAGE \| TRANSFER_DST | 1c | 1c | 1b | SCENE |
| 2 | `MESHES` | SSBO ClusterMesh[]; STORAGE \| TRANSFER_DST | 1c | 1c | 1b, 2a, 2b | SCENE |
| 3 | `GROUPS` | SSBO ClusterGroup[]; STORAGE \| TRANSFER_DST | 1c | 1c | 1b, 2b | SCENE |
| 4 | `REQUESTS` | SSBO header + ClusterRequest[]; STORAGE \| TRANSFER_SRC \| TRANSFER_DST | 1c | **1b shaders** (append), 1c (reset) | 1c (readback) | CULL |
| 5 | `PAGE_USAGE` | SSBO uint[pool pages]; STORAGE \| TRANSFER_SRC \| TRANSFER_DST | 1c | **1b shaders** (frame stamp) | 1c (readback, LRU) | CULL |
| 6 | `INSTANCES` | SSBO ClusterInstance[], per slot; STORAGE (host-visible or staged) | 1b | 1b (host) | 1b, 2a, 2b | SCENE |
| 7 | `VIEWS` | SSBO ClusterView[16], per slot; STORAGE, host-visible | 1b | 1b (host) | 1b, 2a, 2b, 2c | CULL |
| 8 | `SELECTED` | SSBO ClusterSelected[] (per-view slices); STORAGE | 1b | 1b | 1b draw, 2a, 2b | CULL |
| 9 | `COUNTERS` | SSBO uint[]; STORAGE \| INDIRECT \| TRANSFER_DST | 1b (documents offsets) | 1b | 1b, 2a, 2b | CULL |
| 10 | `DRAWS` | SSBO; STORAGE \| INDIRECT | 1b | 1b | indirect draw / mesh tasks | CULL |
| 11 | `STATS` | SSBO uint[32] per slot (256 B stride) | core | everyone (`CL_STAT_ADD`) | core readback | CULL / READBACK |
| 12 | `JOINT_STRETCH` | SSBO {rows, pad×3, float[rows·128]} | 2a (core placeholder: rows 0 → stretch 1) | 2a | 1b, 2b | RIG |
| 13 | `HIZ` | COMBINED_IMAGE_SAMPLER | renderer import (core writes) | – | 1b (`CLV_OCCLUSION`) | – |
| 14 | `PICK` | SSBO {count, pad×3, tier-0 records}; STORAGE \| INDIRECT \| TRANSFER_DST, 4096 records (80 KB, made at init) | 1b (#702 integration) | 1b pick filter | the GPU pick's cluster draw | CULL |
| 15 | `INST_CUT` (#1011) | SSBO uint[8 per ClusterInstance row]: {count, hash a, hash b, tris, main view frame + 1, 0, 0, 0}; STORAGE, pow2 steps up to kMaxInstances rows (4 MiB) | 1b (`ensureInstCut`, beginFrame) | 1b main-view selection (shared instances, `CLA_FLAG_RT_SUMMARY`) | 2b (ranked shared cut) | CULL |
| 16 | `SKIN_OUT` | SSBO uint[]: the skinned cluster arena (see below) | 2a | 2a (cluster pre-skin, weld) | 1b draw (VS / mesh, pre-pass + G-buffer), 2b (rigged CLAS input + hits of the instances the shared rig cut covers; RT consumer binding 14) | RIG |
| 17 | `SKIN_SCRATCH` | SSBO uint[]: header (`CLK_*`) \| posed-bound margin per ClusterInstance \| map per selected entry \| owned bitset \| weld grid; + INDIRECT (pre-skin dispatch args) | 2a | 2a (prep, pre-skin, weld) | 1b selection (margins), 1b draw (map, eye, cap), 2b (margins of the rigged RT cut, the map of the shared cut) | RIG |
| 18–23 | reserved (placeholder SSBO bindings exist; 2b uses none: its buffers live in its own set) | | | | | |

**Bindings 16 / 17 (2a, #747; layouts in `clean_cluster_skin_common.glsl`, C++ twin
`llcleanclusterskinvk.h`):**
- `SKIN_OUT` current region: per arena slot `s`, 64 `SkinVertex` (20 B, `clean_skinvert.glsl`)
  at word `(s·64 + local)·5`; `local` = the vertex's index in its cluster. **Positions, normals,
  tangents (and the skinned boxes) are in the CHARACTER's space** (2026-10-14 step 2, below), not
  eye-relative. Then the previous-position region (integration of 2a + 2c): 3 floats per vertex at
  word `cap·64·5 + (s·64 + local)·3` (`cap` = header `CLK_CAP`), EYE-RELATIVE to this frame's
  eye, written only when the slot is skinned this frame on a motion-vector frame (their motion
  pre-pass is the only reader; a reused slot's previous position comes from its row's previous
  frame, below).
- Tail of `SKIN_OUT` (all sized by `cap`; GLSL `clSkin*BaseC`, C++ twins in
  `llcleanclusterskinvk.cpp`): skinned boxes (8 words / slot), per-avatar box unions (8 words /
  row, `CL_SKIN_AV_ROWS` = 1025 rows), **slot META** (8 words / slot, persistent: key = render
  instance, cluster mesh, mesh generation, skin id, mesh-local cluster id = page header
  `page_index`·64 + cluster index; frame last in the cut; pose generation it was skinned at; frame
  last skinned), the per-frame **free list** (`cap` words) and **key hash** (2·nextPow2(cap)
  words), the **character table** (36 words / row), the per-row **pose generations** (1025 words)
  and the rows' **new-cluster bits** (33 words). Any (re)creation of the arena -- and a dropped
  recording -- fills the meta `CL_NONE`: every slot free, everything re-skinned once.
- **STABLE SLOTS** (step 1): a cluster keeps its slot while it stays in the cut, + a 4-frame grace
  (`kSlotGrace`). Per frame, inside `recordPreskin`: ARGS (prep mode 3: header, indirect args,
  clears incl. the hash) → SWEEP (mode 4, one lane per slot: live → hash, else → free list, meta
  cleared) + CHAR (mode 6, one lane per row) → ALLOC (mode 5, one lane per selected entry,
  indirect: hash hit = kept slot, miss = a free slot, none = `CL_NONE` + `CLK_OVERFLOW`) → map.
  Neither the ClusterInstance index (re-assigned by the gatherer every frame) nor the
  `cluster_ref` pool slot (changes on evict / reload) is part of the key.
- **CHARACTER SPACE** (step 2): the frame of the avatar's PELVIS (joint 0 of its joint-table row,
  columns normalised = `clean_preskin.comp rigidRot`, the #632 held-frame cache's and the RT
  avatar BLAS's frame). Per row: `M`, `t` (= pelvis − this frame's eye, formed in float in agent
  space as `clean_skin_palette.comp` does), `Mp`, `tp` (the previous frame's joints, relative to
  THIS frame's eye; = `M`, `t` without joint history), `M⁻¹`, valid. **Every reader applies it:
  eye-relative = `M·local + t`, normal / tangent `M·n`** -- the cluster draw VS + mesh tier
  (`clusterSkinnedVertex`), `clRigEntryInView` (task shader, rig-list culls: the box → a
  conservative eye-relative AABB, `clCharBox`), the alpha order (box centre), 2b's shared-slot
  copy (mode 9). The weld runs in character space (rigid: same distances / angles). Row `none`
  (an instance without `CLI1B_AVATAR`: unresolved skin) = identity (eye-relative, never reused,
  not welded). One row per character for every mesh / level: seams stay watertight.
  NOT BIT-IDENTICAL to the eye-relative arena: one float rotate + translate round trip
  (~1e-7 × the eye distance) and the octahedral normal quantised in the local frame.
- **HELD POSES** (step 3, `RenderVKClusterSkinReuse`): the renderer passes per joint-table slot
  `avatarPoseDirtyBit`'s clean rule (`LLClusterSkinPrevIn::pose_clean`: anim store built this
  frame + `slotPoseClean` + skin meta unchanged + the 64-frame stagger); the module keeps a POSE
  GENERATION per row (a new globally unique value whenever not proved clean, and for every row
  after a frame without a pre-skin), uploaded each pre-skin. A slot is REUSED (not skinned, no
  previous positions written, its box joins the avatar union) when meta [6] = its row's
  generation and the row has a real frame; with the weld on, only if no cluster of the row is
  new to the cut this frame (reused slots hold post-weld positions: a row with a fresh cluster is
  re-skinned whole). The draw takes a reused slot's previous position as `Mp·local + tp` (meta
  [7] ≠ this frame). A KEPT slot that is not reusable (skinned at another generation -- it left the
  cut or the view for a few frames while the row's pose moved on -- or in a degenerate frame) marks
  its row like a new cluster in ALLOC (#798, 2026-10-16): with the weld on the whole row re-skins.
  Before, such a slot re-skinned alone: it was inserted into the weld grid without its partners
  (reused slots insert nothing) and its partners kept welds made without it -- an open seam until
  the row's next generation (≤ 64 frames).
- The map: word `header[CLK_MAP_BASE] + sel` (absolute selected index) = the entry's arena slot
  `s` (ALLOC), or `CL_NONE` (static, not drawn for real, arena full, or `sel ≥ CLK_MAP_CAP`).
  It is written for the main view's slice only (`sel_base` from `cl_views[view]`), so 2c's
  per-role arena slices need no offset handling: the map covers the whole arena (1b's
  `arenaEntries()`).

**1b counters / draws / views tail (#702 integration):** `CL_B_COUNTERS` has 64 words per view (96 since the rigged alpha lists, §9)
(`kViewWords`, was 32): +20 `kCntRigDraws` (4 words, the shared rig lists' record counts, main view
only), +24 `kCntRigTasks` (4 × 4 words, their mesh-tier task commands). `CL_B_DRAWS` ends with the
SHARED RIG LISTS: rig index r = list × 2 + bucket at record `rigBase + r × rigCap` (rigBase =
ceil((aux_base + arena) / 5); rigCap 4096 .. kMaxArena, grown from the read-back demand like the
arena), same record format, firstInstance = the MAIN view's absolute selected index.
`ClSelectAux` is 400 B since the 1024 avatar slots (was 192 / 208 with the alpha lists, §9): + `rig` {first rig record, rigCap, bit 0 = the main view selected the
shared cut, pad} + `rig_bits[16]` (per list 1024 avatar bits = `kAvatarSlotCap`, llavatarslots.h / clean_av_slots.glsl). 1b-private `ClusterInstance::flags`:
2048 `CLI1B_AVATAR` (bits 16..25 = the wearer's joint-table slot, 10 bits since the 1024-slot cap), 4096 `CLI1B_DYN_CASTER` (a
moving caster, #567). `SelectPush.rig_bounds` bit 1 = this secondary view skips moving casters.

Rules for these buffers:
- Staging rings are `VKMEM_STAGING` and readback rings are `VKMEM_READBACK`.
- Reuse the existing VKMEM categories; do not add new ones.
- Every module keeps its own live / cap / peak and reports it in `statsLine`.

## 9. Interfaces (exact signatures in the headers)

**Core** (`LLCleanClusterVK`):
- `init`, `destroy`, `tick`, `recordCull`, `recordDraw`, `idleRelease`, `statsLine`.
- Services: `ctx()`, `caps()`, `env()`, `setLayout()`, `set(slot)`, `slot()`, `frame()`,
  `setBuffer()`, `retire()`, `setStages()`, `select()`, `stream()`.

**1b → 1c:** `stream().acquireMesh(const LLClusterKey&, uint32_t src_slot, uint64_t frame, float importance, uint32_t group, uint32_t* hint = nullptr) -> MeshRef{mesh, generation}`.
- `hint` (in / out): the directory index the caller cached for this key. A hint whose entry still
  holds the key (not Free) skips the key hash; the index used is written back. 1b calls it once
  per key per frame through its per-key cache (incremental gather, below).
- `importance` / `group` are the build priority of a cache miss: for a rigged row, the avatar's
  height over its distance (our own avatar 1e6) and its joint-table slot; for a static row, its
  scale over its distance. When one mesh is acquired several times in a frame, the largest
  importance wins.
- Returns `kNone` while the mesh is loading, building or uploading its roots (2026-10-14: no
  longer waits for the `kWarmPages` warm pages -- they are requested right after the roots). The
  instance is NOT DRAWN meanwhile in the cluster-owns-rigged mode (§1); a static stays legacy.
  Time to drawable (first acquire -> roots resident) per mesh, p50 / p95 / max split cache / built:
  `| ttfd n N p50 X p95 Y max Z ms (cache .., built ..)` on the 10 s CLOD line. Registration: 96
  per frame (a count-limited one retries next frame, a room-limited one after 15).
- Calling it marks the mesh live this frame.
- **Page sources** (1c): a mesh's pages come from its build payload while it is held, else
  from the disk cache. The payload is released (after `kPayloadHoldSec` or over the payload
  budget) only once `pageSource` says the disk holds that build. A payload the cache cannot
  serve is kept while the mesh lives (freed on liveness, reported as `no disk`) and released
  only under RAM pressure. A wanted page with no source stays ABSENT (its parent draws) and
  never blocks other meshes' loads; only a full read queue ends a frame's cache loads.
- **Rebuilds of a Live mesh** (1c, `maybeRebuild`): when the face publishes a higher creator
  LOD than the DAG was built from (a face first seen far away is built from the LOD resident
  then), once per LOD step; and, with RAM calm, when a wanted page has no source. The mesh
  stays Live meanwhile. The same DAG (header hash) only brings the payload back; a different
  one re-registers (the instance is not drawn for the frames its roots take).

**1c → 1a (cache):**
- `LLCleanClusterCache::requestMeta(key, out) -> State`
- `requestPages(key, pages, n, ticket)`
- `pageSource(key, header_hash) -> Ready | Full | Pending | None`: whether the disk can serve
  the pages of that build. `None` = never (a lower creator LOD build is not persisted; evicted;
  dropped; another build's file).
- `takePages(out)`
- `store(mesh)`
- `drop(key)`
- `poll()`

**1c → 1a (builder):**
- `LLCleanClusterBuilder::request(key, src_slot, frame, Prio{importance, group, rebuild}, bool* no_source)`.
  The queue order is:
  1. band: first builds of rigged faces, then first builds of statics, then rebuilds of Live meshes;
  2. importance, highest first;
  3. group (the avatar), so its faces build as one set (an avatar's meshes arrive together; since
     2026-10-14 each mesh draws as soon as its roots are resident);
  4. arrival.

  A repeat call is O(1). It updates the priority (the queue is re-sorted only past a 1.25x change)
  and, while the job is still queued, moves it to a higher-LOD slot that the face now publishes:
  one build instead of a build plus an upgrade rebuild. The stream repeats it for Building meshes
  whose priority or slot changed. `no_source` = refused because there is no CPU mirror / LOD yet.
- **RIGGED DAG SOURCE = the highest creator LOD (#798, 2026-10-16).** In the cluster-owns-rigged
  mode `clusterTick` sets `LLVOAvatar::setVKClusterRiggedLOD(RenderVKClusterRiggedSourceLOD,
  default 3)`; `LLVOVolume::calcLOD` then gives every rigged volume that LOD at every distance (no
  #559 / #734 pressure cap; HUDs as before), so LL fetches it and the ingest publishes it. The
  DAG's cut does the screen-size reduction; creators match the seams between meshes on LOD 3 only
  (a LOD 2 source left an avatar's leg / rump seam with no weld partners). A Live mesh built from a
  lower LOD is upgraded when the higher slot appears (`maybeRebuild`), and the swap is CLEAN: the
  new DAG registers as a SHADOW directory entry (`startShadows`, not in the key map, never
  acquired; liveness / priority follow its live entry) and takes the key over in the frame its roots
  are resident (`shadowSwap`, before the gather) -- no frame without the mesh (the in-place swap
  remains only when no directory entry is free). The stream line reports `src lod 3/2/1/0` (live
  meshes by DAG source) and `upgrading` (shadows waiting for their roots). Cost: download + LL
  volume RAM + the store's CPU mirror of LOD 3 for every rigged mesh in view until its DAG is
  on disk (then freed, §1 "CPU mirror release"; no draw cost: no legacy GPU geometry); pressure
  acts through the cut tolerance (2c), not the source.
- `pump(budget_ms)`: budget_ms is RENDER-thread time (finished builds and source copies,
  0.25 .. 8 ms). The workers are not metered. Jobs in flight are at most max(4, 3 x threads),
  with at most 256 MB of source copies in flight. Until 2026-10-13 this was a 4 ms-per-frame budget
  of worker time for the whole pool, so about one build ran at a time.
- The stream registers up to 4096 meshes that are not yet live (`kMaxPending`, = the builder's
  queue bound). It was 1024, and that held a crowd's backlog in arrival order. A fresh build is
  validated on its worker (`runBuild`); the stream re-checks only `metaSane` for it, while a cache
  load gets the full metadata `validate()` on the render thread.
- `takeFinished(out)`
- `validate(mesh, why)`

**2a / 2b ← 1b:**
- `select().viewSlice(view) -> {sel_base, sel_cap, flags}`.
- The view's selected count and dispatch args live in `CL_B_COUNTERS`, at offsets 1b documents
  in its header.

**2a → everyone:** `setBuffer(CLB_JOINT_STRETCH, …)`, `CLB_SKIN_OUT`, `CLB_SKIN_SCRATCH`.

**2a ↔ 1b / core / renderer** (#747, `vkgfx/llcleanclusterskinvk.h`, owned by the core: `skin()`):
- Gather hooks in 1b's gatherer: `gatherBegin`, `avatarOfSkin`, `noteRow`, `noteInstance`,
  `gatherIncomplete`, `admit(allowed, out)` → the admitted instances get `CLI1B_DRAW`. 2026-10-14:
  PER MESH -- every rigged ClusterInstance is admitted as soon as it exists (no "every mesh of the
  avatar" gate, no arena admission); held back only for: module / setting off, no palette, our own
  avatar hidden (mouselook), a broken-off gather, missing pre-pass pipelines with per-pixel readers
  (`RenderVKClusterRigged 1`; warned after 600 frames). In the cluster-owns-rigged mode the
  gatherer sets the legacy-exclusion bit of EVERY eligible rigged row itself (drawn or not) and the
  exclusion is not gated on the cluster G-buffer draw having run. The gatherer calls the hooks in
  row order over its candidate list (every eligible rigged row: `noteRow`).
- **Skinned arena = demand** (2026-10-14): the slots the sweep kept (`CLK_LIVE`, includes the
  4-frame grace) + the ones the cut asked for anew (`CLK_ALLOC_N`, granted or not), peak of
  ~2 x 5 s + 1/4 + 1024, doubled at once on an overflow, never
  past the main slice's map, capped by the arena LIMIT. A drawn rigged cluster without
  a skinned slot that frame is NOT drawn (collapsed in `clusterVertex`), never the rest pose. (The
  old upper bound -- every admitted mesh's whole DAG -- held 129k of 131k clusters for 21k skinned
  at PEAK and kept 14 avatars legacy.)
- **Arena pressure / whole avatars (#1010).** The pony party (~87 avatars, 100-400k verts each)
  overflowed the old fixed 256 MB cap by up to 40k clusters a frame, and the free-list race left
  random parts out (missing parts, popping eyes). Now: (1) LIMIT = `RenderVKClusterSkinArenaMB`, 0
  (default) = 1/16 of the VRAM budget (`LLClusterFrameIn::vram_budget`, 128 MB .. 4 GB), x the
  VRAM ladder depth (3/4, 1/2, 1/4; soft), the binding range, growth only into `vram_avail` while a rung is engaged, a
  failed growth held ~2 s. (2) ARENA PRESSURE: skin's controller (read-back demand vs the limit)
  sets `control().setArenaPressure(P)`; `avatarTolScale(..., &arena)` returns the per-avatar term
  2^clamp(P - L, 0, 8), L = log2(on-screen height / 1 % of the viewport) in 0..6, 9 for your own
  avatar; the gatherer multiplies it in after the alpha cap (`ci.tol_scale` may exceed 1, bound
  `kMaxTolScale` 256; the rigged RT cut reads the same value). Every mesh of an avatar coarsens by
  the same factor. P +1 per 0.15 s while SHORT at the limit, +0.25 over 92 %, -0.5 per dwell (2 s,
  doubled per re-raised release, up to 16 s) under 65 %. (3) WHOLE-AVATAR ADMISSION on the GPU:
  ALLOC (prep mode 5) keeps slots and only counts new entries per avatar row; GRANT (mode 7, one
  workgroup) refuses, when the new entries exceed the free list, every row past it in admit()'s
  PRIORITY ORDER (own avatar, then importance; uploaded into the scratch's PRIORITY region after
  the layer bits); TAKE (mode 8) gives slots to the rest and maps every entry of a refused row
  CL_NONE. `CLK_OVERFLOW` now counts partial drops only (expected 0). Header 44..47 = PRIO_BASE,
  SHORT, REFUSED_AV, HIDDEN; word 19 = ALLOC_TAKEN. Logged: per-episode warnings and the 10 s
  `CLOD skin arena:` line (dropped, hidden av, coarse av, pressure, live/cap/peak MB, limit).
- `SelectPush.rig_bounds` (was `pad`): 1 = this frame's posed-bound margins are written; the
  selection then inflates a rigged instance's spheres and distance margin by
  `clRigMargin(instance)`. 0: a drawn-for-real rigged instance is never culled and takes LOD0.
- Renderer: `setFrameInputs(LLClusterSkinIn)` in `clusterTick` (palettes, skin meta, joint table,
  legacy work list, counts, eye, own-avatar hiding, pre-pass readers);
  `recordPreskin(cmd, main_view, LLClusterSkinPrevIn, mark)` after the legacy pre-skin and weld.
  `LLClusterSkinPrevIn::palette` = the device address of the renderer's previous-frame palettes
  when the legacy pre-skin built them this frame (u_prev 1), `joints` / `joints_rows` = the
  previous joint table they were built from (the characters' previous frames), `pose_clean` =
  the per-slot held-pose proof (step 3), `motion` = a motion-vector frame; `mark` = the
  renderer's GPU sub-timer.
- **`RC_CLUSTER` (1024)**, rig-cache state word: the main cull (`clean_cull.comp`, cluster mode)
  sets it on an owned rigged instance, which stays in every bucket and work list. The rigged
  passes that draw the cluster path's avatars themselves drop it: those pushes set
  `u_sky_zenith.w = 1` (`clean_draw_rigged.vert`) -- the main view's, and (#702 integration) a
  shadow / capture pass whose shared rig list is drawn this frame and the GPU pick when its
  cluster pick runs. **Legacy-skin skip** (§11): when no consumer needs a switched avatar's
  legacy vertices this frame (av_vis bit 27 `AVV_CL_SKIP` / `kAvVisClSkip`), the main cull does
  not keep its owned rows at all: no bucket, no off-screen region, no pre-skin work item (state
  word = `RC_CLUSTER` only, skin-cache key invalidated).
- **The switch** (`RenderVKClusterRigged`, per mesh): 1 = a mesh draws when the cluster pre-pass
  pipelines exist (`select().prepassReady()`), or, without them, while nothing reads the main
  view's pre-pass per pixel; 2 = always; 0 = never (rigged meshes then stay on the legacy path:
  the cluster-owns-rigged mode is off).

**2b RT (#748, `vkgfx/llcleanclusterrtvk.h`, owned by `LLCleanRTVK`):**
- **RT cut:** its own pass (`clean_cluster_rt.comp` mode 0) over 1b's ClusterInstance list, not a
  1b view: the RT cut must be whole (no frustum / cone / Hi-Z culling: off-screen casters) and
  per instance (contiguous ranges, a per-instance reference camera with
  `RenderVKClusterRTHysteresis`, a cut hash for rebuild / refit), which 1b's per-view slices do
  not give. Its own cut arena (module set binding 2), so 2c's role slices do not apply. The
  projection's `ClView::lod_cam` is the reference camera (margin 0). Tolerance = 2c's
  `CLROLE_OTHER` (`control().roleTolPx`: `RenderVKClusterRTTolerancePx`, never pressure-scaled).
- **Static cost control (#748 perf pass):** with `RenderVKClusterRTStatic` (default 1; 0 = every
  static keeps its legacy BLAS in RT) a static instance takes part only when its importance (world
  sphere radius / distance) passes the CPU's admission threshold (`RP_STATIC_MIN`; an admitted
  one keeps its place down to 0.7 x it). The threshold rises (every 8 frames) while the CLAS pages
  the admitted cuts use + want do not fit an arena that already has all its room -- in ONE step
  (#748 static pass): mode 0 counts the pages used + wanted per importance bucket (1.5 per octave
  from 2^-14, a page at the bucket of the first instance that stamped it), and the threshold
  becomes the lowest bucket edge whose buckets above it fit 4/5 of the arena -- and falls back
  towards 0 (= all, x0.8 per 8 frames) while they fit with a quarter to spare: an instance that
  could not become ready pays no selection and keeps its legacy BLAS. The static CLAS arena is
  sized FROM THAT DEMAND (16 pages with no static on the cluster path; all the room while the
  threshold limits them) and lives IN CHUNKS of 32 pages (#748 static pass; chunk table in module
  binding 33): growth adds at most 2 chunks per frame -- no CLAS moves or is lost, no large
  allocation lands in one frame (the single re-created arena cost 6-12 ms of CPU per step, the
  `vk_rtClusterPlan` hitches, and every CLAS); a pool growth keeps the CLAS (the per-slot tables
  are copied on the GPU); it is reset only when the statics left the cluster path (~5 s) or the
  room stayed under it (~2 s). A page no cut used for 60 frames is freed. The rigged reservation
  never eats the static floor (16 pages): a small cap cuts the rigged arena instead of failing the
  tier. An admitted instance whose reference camera did not move reuses last frame's cut (two cut
  arenas alternate per cut epoch, module binding 32; every entry's page must still be resident,
  else it walks; every 32nd frame per instance it walks anyway for finer pages that became
  resident) instead of walking its pages.
- **Static cut perf pass (2026-10-14):** (a) a static RT cut is never finer than the raster draws
  it: its tolerance = max(`RenderVKClusterRTTolerancePx`, `RenderVKClusterRTStaticRasterTol` (1) x
  the controller's base tolerance x 2^pressure LEVEL) -- the level, not its easing, so cuts and
  persistent BLAS change only when the level steps (at PEAK the raster ran at 8 px while statics
  traced at 2 px: 1.53 M RT tris / 68.7 k clusters, ~4x the drawn surface, and a caster off the
  rasterized receiver). (b) The static CLAS arena has a budget, `RenderVKClusterRTStaticMB` (0 = a
  quarter of the cluster RT cap, 64 .. 512 MB), as the ceiling of its room: before, it took all the
  room the cap left while admission limited the statics (1078 MB at PEAK vs 200 MB of compacted
  legacy BLAS for every static in range); admission keeps the demand inside it. (c) Mode 0's
  page stamps (one per cut entry) read before their atomics (64-way same-address serialization).
  The RT stats line shows `static tol T px, admit >= ..., CLAS budget N pages`.
- **Persistent static cluster BLAS (#748 static pass, `RenderVKClusterRTStaticPersist`, default
  on, live):** a static instance's cluster BLAS lives in a HEAP block (explicit destinations,
  `PREFER_FAST_TRACE`; 22 size classes of 1..2048 clusters, per-class GPU free stacks + a bump
  pointer; bookkeeping + per-CLAS-page build stamps in module binding 34; the instance's BLAS
  state in its hysteresis rows, `ST_STRIDE` 5). Mode 4 KEEPS it while the instance's cut hash, mesh
  and every CLAS it references (build stamps no newer than the BLAS) are unchanged -- a still
  camera builds no static BLAS and its override rows keep their addresses (no PTLAS writes);
  otherwise REBUILDS it into a new block (the old one freed after the frame's allocations, mode
  21); while the new cut is not ready (a CLAS missing, no block) the PREVIOUS BLAS keeps tracing
  as long as no CLAS was freed since it was built (no flip to the legacy BLAS). The heap is sized
  from the read-back live units (x2, inside the cluster RT memory), grown or re-packed (a new
  epoch: one frame of full static builds) on missing blocks, shrunk after ~10 s under a quarter.
  Rigged BLAS keep the per-frame implicit build (their CLAS change every frame). Not done:
  compaction (classes waste <= 1.5x; the explicit build writes the real sizes, a MOVE_OBJECTS pass
  could pack them).
- **Eligible:** static instances drawn for real (`CLI1B_DRAW`, not `CLI_RIGGED`); and, with
  `RenderVKClusterRTRigged` (default 1), the rigged instances of the avatars 2a switched
  (`CLI_RIGGED` + `CLI1B_DRAW`) -- **rigged cluster RT**, `llcleanclusterrtvk.h` RIGGED:
  - **Cut:** mode 0 with the MAIN VIEW's projection (`cl_views[LLCleanClusterVK::mainView()]`:
    2c's LOD camera + margin, held px_scale, post-controller tolerance) and 1b's posed-bound
    margin (`clRigMargin`; LOD0 without margins, as 1b), un-culled. Over the clusters the raster
    draws it is the raster's cut (the rule is per group; culling never changes it); off-screen
    limbs get the cut the same rule gives them. Deliberately not `RenderVKClusterRTTolerancePx`:
    any other projection leaves the traced surface up to the larger cut's error off the
    rasterized receiver (the self-shadow mismatch again).
  - **Vertices: the shared skinned cut first** (CLAS tiers): an instance whose whole RT cut is in
    the main slice's shared rig cut (below; mode 11 counts its skinned entries and hashes their refs = its cut's size and hash)
    takes 2a's SKIN_OUT vertices (welded; mode 12 writes its slot headers, mode 9 copies them into
    the RT arena relative to the RT origin since #748 perf pass 2; the CLAS and hits read the arena) -- one skinning for raster, shadow
    maps and RT. **RT skinned arena** (module set binding 26 = RT consumer set binding 13) for
    the rest (not covered by a rig list, overflow) and for tier 0: mode 9 skins the cut with
    2a's math (statement for statement; no weld) from the renderer's palettes (module binding
    28), eye-relative: 64 vertices x {xyz fp32, skinned normal} per slot; a header
    `{ref, vpage | SKIN_OUT slot, k | tris << 16, instance | bit 31 shared}` per slot.
  - **AS, every tier:** CLAS tiers: the arena is handed out PER AVATAR, whole or not at all
    (mode 0 sums each avatar's cut sizes, mode 13 admits avatars while they fit -- last frame's
    admitted ones first, then joint-table slot order, so a newcomer never evicts a covered avatar -- mode 14 places each instance inside its avatar's range: no holes): an avatar that
    does not fit gets no slot and no CLAS -- its switched groups keep the legacy BLAS -- while
    every avatar that fits is covered completely, so `clusterRigCovered` (per avatar) confirms for
    it (the old first-come compare-and-swap spread ~400 over-capacity clusters over every avatar:
    none was ever covered). The capacity follows the read-back demand (+ a quarter, 1024-slot
    steps, shrunk after ~5 s under half), inside the rigged share of the cluster RT memory
    (MEMORY below); one CLAS build per frame (`c_cl_rig_clas`; explicit destinations per slot, kept avatars skipped -- #748 perf pass 2 below -- geometry index = the
    cut entry k), taken into the instance's cluster BLAS with the static ones -- no lag. Tier 0:
    a KHR BLAS (a geometry per cluster) BUILT from the harvested cut into a persistent arena
    range, REFIT every frame from this frame's skinning (`c_cl_t0_rig`); the LOD lags
    kFrameSlots frames + the build budget, the pose never.
  - **TLAS:** the rigged plan in `llcleanrtvk.cpp` keys the switched faces into their own groups
    (mask-class byte bit 0). Pass B gives such a group its members' OWN cluster records
    (translation by the palettes' eye, rigged mask class, never facing-culled, `rt_geom`
    `RT_GEOM_CLUSTER_RIG`) only when every member has an override this frame (mode 4; refit
    frames: and may be active), else nothing -- never both, never half a group.
    **Cluster-only RT (2026-10-13, owner decision: with cluster LOD on there is no legacy
    rendering, not even transitional):** while the cluster RT traces switched avatars
    (`LLCleanRTVK::clusterTracesSwitched`: `RenderVKClusterRT` and `RenderVKClusterRTRigged` on,
    the module active at any tier), a rigged face ON THE CLUSTER PATH
    (`LLCleanClusterSelectVK::rowOnClusterPath`, the hook for eligibility) is traced only from its
    cluster cut: switched = a cluster-only group (no legacy rigged BLAS, rig copy or rig_pos range,
    no fallback, no confirmation wait; cluster data not ready that frame = nothing traced); not
    switched yet = not traced at all (pop-in accepted). Only faces outside the cluster path
    (flexi, system body, non-mesh; alpha-blend rows are on it since 2026-10-14) keep a legacy BLAS, and only they count for the
    legacy avatar budget / drop. Dropped over budget, the plan keeps the switched faces alone and
    pass A keeps only the worn rigid attachments of avatars traced from their cut (group map
    words 8..15, `CLRIG_AV_TRACED`; `joint_count` 0). Cluster LOD off, or no cluster RT for
    switched avatars: the legacy plan exactly as before.
- **Shared buffers:** reads `POOL`, `PAGE_TABLE`, `MESHES`, `GROUPS`, `INSTANCES`,
  `JOINT_STRETCH`; stamps `PAGE_USAGE` with `LLCleanClusterVK::frame()`, the value 1b stamps.
- **RT sets:** RT consumer set (`clean_rt_common.glsl`) 15 bindings: 11 = the cluster page pool,
  12 = the tier-0 hit-cut table, 13 = the RT skinned arena, 14 = 2a's `SKIN_OUT` (rigged cluster
  hits) (dummies when inactive); binding 0 is a PARTITIONED acceleration
  structure for the whole session when `RenderVKClusterRT` ≥ 2 at RT init and the device has
  `VK_NV_partitioned_acceleration_structure` -- which `vk_device.cpp` enables only when
  `RenderVKClusterLOD` is on at startup (`setClusterRequested`; never through
  `RenderVKClusterDeviceFeatures`), so with it off at startup
  `RenderVKClusterRT` changes nothing in the TLAS or the RT sets. TLAS pass A set
  (`clean_rt_instances.comp`): 24 bindings, 22 = the override rows `{BLAS lo, hi, hit-cut base,
  first vertex | 0x80000000}` per instance id (rigged: `{BLAS lo, hi, first arena slot, first
  vertex | 0x40000000}`; zero dummy = none), 23 = the switched-group map (host-written per slot by
  `tlasRigMap`; zero dummy = every group legacy). Hits: `rt_geom.z` bit 30 marks a cluster
  instance, bit 28 a rigged one (with bit 31: every rigged rule of the consumers applies).
- **Tiers:** 2 CLAS + PTLAS, 1 CLAS (cluster BLAS from the cut's CLAS every frame), 0 per-cut KHR
  BLAS from a readback (`RenderVKClusterRTBuildsPerFrame`). Failures step the tier down for 600
  frames; `RenderVKClusterRT 0` keeps the legacy BLAS with clusters on.
- **PTLAS partitions (2026-10-22, `RenderVKClusterPTLASPartition`, default 1 = spatial):** the
  partitions were `id / 512` with ids from a lowest-free allocator (arrival order), so every
  partition spanned the scene: 3-35x slower traces (2026-10-14; why `RenderVKClusterRT` defaults to
  1). Now mode 7 (`clean_cluster_rt.comp ptlasSpatialPartition`) puts each written record in the
  partition of its translation's grid cell (`RenderVKClusterPTLASCellM`, 64 m), hashed into up to
  1024 partitions; disabled records, instances whose largest axis scale exceeds a cell and a full
  partition's overflow go to the GLOBAL partition, which can hold every instance (never an
  over-capacity write). Counts per partition and each instance's partition live in module binding
  35 (`mPtlasPart`, filled 0 / CL_NONE with every fresh PTLAS); a record that moves cells is
  written with its new partition (the record changed anyway). The per-partition capacity is the
  largest of cap/4 .. 512 whose size query stays within 2x the id-order PTLAS; none -> id order
  (warned). Logged at creation (`partitioned TLAS for N instances, SPATIAL partitions P x <= M ...,
  X MB (id order Y MB)`) and on the Cluster RT line (`writes to global`). Still opt-in
  (`RenderVKClusterRT 2` at startup) until a fixed-camera A/B against 1. Not done: a partition
  bounds debug view; partition translation ops for origin moves.
- **MEMORY (#748 perf pass):** every arena of the module (static CLAS chunks, cluster BLAS, the
  static BLAS heap, rigged arena + CLAS, PTLAS) lives in ONE cap, `RenderVKClusterRTMemoryMB` (0 = a
  third of the RT budget, 64 MB .. 3 GB -- #748 static pass: the old 1 GB ceiling bound on large
  cards once the RT budget's auto rule became 12% of the first 8 GB of the VRAM budget + 20% of the
  rest, 384 MB .. 8 GB, 2026-10-13; it stays a share, so small cards keep a small cap; manual
  values 64 MB .. 3 GB), allocated exactly (the generic x1.5 growth step made the 358 MB CLAS
  arena 537 MB). RIGGED FIRST: the static CLAS arena is sized around the rigged demand's bytes
  (at most 3/4 of the cap); the rigged arena gets what the static arenas leave. `LLCleanRTVK`
  keeps this share out of its avatar-drop / over-budget rules (they judge the legacy classes
  against the budget minus it): before, the cluster arenas counted as "working buffers" there and
  dropped every avatar and worn attachment from RT (`RT: over budget ... avatars dropped`) --
  then the legacy rigged plan was empty, pass B never ran, the rigged override rows had no
  reader, no avatar cast an RT shadow and no avatar was ever covered. 2026-10-13: the rigged
  plan no longer depends on the legacy class -- dropped over budget (or with cluster LOD on at
  all, above) it still runs over the switched faces as cluster-only groups, so pass B reads the
  override rows; rigged cluster RT is skipped only without a rigged plan at all (`no rigged RT
  plan this frame ... no pass B consumer`: no rigged pre-skin / RT rig buffers). The legacy
  avatar drop judges only legacy avatar BLAS (none exist with cluster LOD on).
- **RT budget headroom (2026-10-13):** on a card whose VRAM budget is above 8 GB the auto RT
  budget also follows the measured headroom: what RT holds + (budget - usage - reserve), reserve =
  max(2 GB, 30% of the budget: under the ladder's soft line and its recovery line), at most 60% of
  the budget / 16 GB, never under the fixed share (12% of the first 8 GB + 20% above); grows
  (+5%) only while the VRAM ladder is calm, shrinks under -10%; the pressure ladder still caps it.
  Cards <= 8 GB: the fixed share only. `RT: memory budget ... HEADROOM: usage U (RT R, other O),
  reserve, ceiling -> B` on every change. Working buffers: rigged refits share 128 MB of update
  scratch per frame (a refit past it waits <= 3 frames), one starved rigged build per frame goes
  over the 96 MB build budget (all of a re-admitted crowd went in one frame: ~1.5 GB of scratch),
  the scratch grows by 1/8 and shrinks to its 300-600-frame peak once over 2x it.
- **Logs (2026-10-13):** a 5 s `RT avatars: cluster cut N (fell back F, cluster-only groups G) |
  legacy BLAS L avatars (+W world groups), switched on legacy BLAS S (...) | state | budget B =
  cluster RT + working + static BLAS + legacy avatar BLAS (+ free); drop at ... | refits deferred,
  starved builds | rig copy: undrawn U mismatched M`; the CLOD A/B line adds `(rt: not tracing)` /
  `(rt: cluster-only, no legacy consumer)`. The rig copy check counts faces the main cull drew
  NOWHERE (RT-probe-only candidates the realtime probe job did not pull in, the phase-2 rigged
  capacity drop) apart from real slot mismatches; only mismatches warn.
- **Receiver-lift gate:** pass B counts the switched faces it traced on their legacy geometry
  (group map word 2); `recordLiftFlag` copies it into the RT skinned arena's word 2 (RT binding
  13) and `clean_rt_shadow_mask.comp` skips the lift's probe ray when it is 0 (valid bit set): the
  probe ray ran for every receiver pixel whenever any avatar was switched. Unknown (no map /
  dummy): the probe runs as before. The renderer turns the lift off while RT dropped the rigged
  class.
- **Rig-list cull (1b draw, every raster tier; #748 perf pass 2):** the shared rig lists are the
  whole un-culled cut of every switched avatar a secondary pass draws. The cluster pre-skin writes
  SKINNED BOUNDS after SKIN_OUT's previous-position region (`clean_cluster_skin_common.glsl`): a
  box per skinned cluster (padded by the weld's max gap) and the union per avatar row (atomics;
  the host zeroes the avatar region before each pre-skin). A rig-list record is tested against its
  AVATAR's box first, then its cluster's (`clRigEntryInView`, the drawing view's four side planes,
  no near / far): one 32 B read per record instead of its 64 skinned vertices. Mesh tier: in the
  task stage (it is the tier's per-view compaction). MDI tiers: `LLCleanClusterSelectVK::
  recordRigCull` (`clean_cluster_rigcull.comp`) compacts the list for ONE view into the culled
  region after the rig lists in `CL_B_DRAWS` (2 x rig cap records, counts at the main view's
  `CLC_RIG_CULLED`), recorded by the renderer (`clusterRigCullView`) right before each scope that
  draws a rig list (cascades, overlay cascades, projector tiles, mirror, CPU and GPU probe faces;
  the draw view by device address in the param ring, GPU-patched entries included); that scope's
  draw passes `LLClusterDrawIn::rig_culled`. One region: each cull waits for the previous view's
  draws. `CLOD views` adds `| rigcull N` (MDI culls recorded).
- **Shadow rig list emission (#748 perf pass 2):** `ClSelectAux::rig.w` = the lists whose RECORDS
  the main selection emits; the renderer emits list 0 only while a shadow map draws rigged casters
  this frame (`clusterShadowMapsRig`: sun cascades not ray traced, or projector shadow maps). The
  list's avatars keep their un-culled cut (shared skinning, the derived RT cut) without records,
  and the legacy-skin skip's sun / projector need is void while no shadow map draws rigged
  casters. (In an RT-shadow region `c_shadow_rigcull` measured ~0.8 ms with no cascade drawn:
  that mark follows `asyncJoin` -- it is the graphics queue WAITING for the async RT build
  (`q_rt_build`), not the legacy shadow rig cull, which records nothing without shadow-map views.)
- **Residency-only selection of undrawn instances (#748 perf pass 3, 1b):** an instance no view
  draws this frame (a registered avatar not switched yet, a static not on the cluster path; not the
  debug overlay) takes NO selected entry: the main view walks its cut only every 4th frame
  (staggered by instance) for page usage (well under the stream's `kEvictMinAge`) and requests,
  secondary views skip it. Before, ~48k entries / 3.4M tris of unswitched avatars were selected and
  run through the pre-skin dispatch every frame for 128k drawn triangles. `CLOD t ... sel` and the
  A/B `new:` figures now count drawn instances only. 2a's legacy-verts statistic (a dispatch over
  the whole legacy work list) runs every 64th frame (`vs legacy verts N (... sampled)`).
- **Persistent rigged CLAS + derived RT cut (#748 perf pass 2, `clean_cluster_rt.comp` header):**
  - DERIVED RT CUT: an instance whose avatar is in a rig list, with no overflow (main slice, 2a's
    map / arena), takes mode 11's gather (count, both hashes, triangles) as its RT cut: no page
    walk in mode 0; mode 12 writes its cut entries. `derived cuts N` on the Cluster RT line.
  - Positions in the RT skinned arena are relative to the RT ORIGIN (`RP_ORIGIN`, moved only
    when the eye gets 8 m away or jumps), shared slots COPY 2a's SKIN_OUT vertices into the arena
    (hits read only the arena now; RT binding 14 unused), and pass B translates by the origin
    (group map words 4..7).
  - Every slot has an explicit CLAS destination (`RP_CLAS_BASE + s * RP_CLAS_STRIDE`, the
    explicit per-CLAS worst case); the build covers only the slots whose headers were written
    this frame (per-slot stamp). An avatar is KEPT (no copy, no build) when the CPU proves its
    skinned world positions unchanged since the previous viewer frame (`FrameIn::rigStill`:
    `LLCleanAnimStore::slotWorldClean`, skin meta clean, RT ran last viewer frame), its
    signature (instance ids, cut sizes + hashes, shared flags) equals its last build's, it was
    admitted last frame, and nothing was re-created / the origin did not move (`RP_KEEP`). Mode 13
    places kept avatars at their ranges and first-fits the others around them; fragmentation
    that would leave an avatar out repacks that frame (nothing kept). The animation update-rate
    LOD (held poses between animation frames) therefore rebuilds a held avatar only on its
    animation frames. `avatars kept K rebuilt R, CLAS built N / frame` on the Cluster RT line.
  - Not done: avatar-local CLAS (a walking avatar with a held pose still rebuilds every frame;
    the legacy BLAS refit gate skipped those), trace-optimised CLAS for far avatars. (Coarser RT
    cuts for mid / far avatars: #1008 below; the near band keeps caster = receiver.)
- **Rigged RT cadence (#1008, `clean_cluster_rt.comp` header, `RenderVKClusterRTRigCadence`; 0 =
  the above exactly, the default):** the RT cut and the rigged CLAS no longer follow the raster
  every frame -- ONLY for avatars `LLCleanRTVK::classifyNoSelf` tagged no-self-shadow
  (`FrameIn::rigNoSelf` -> `RP_NOSELF`; hysteresis + 30-frame dwell; their records carry the
  owner tag, their receivers skip their own geometry on every frame). Every other avatar and every
  slotless rigged instance (animesh row 256) stays near / exact; with no tagged avatar traced the
  cadence is off for the frame (no band / schedule work). The coarse cut is capped at 0.1 m past
  the raster error (half the receiver-identity probe window), the pose-skip threshold too. (The
  first cut, d451a121fc, let untagged avatars go coarse / stale: their receivers met their own
  stale copy on kept frames and the exact one on rebuild frames -- self-shadow flicker with the
  period; slotless animesh shared row 256's band, whose multiplier flipped as different objects
  were nearest.)
  - BANDS per avatar from its nearest instance (mode 0 -> mode 13, 10 % dead band): near / mid /
    far (`RenderVKClusterRTRigNearM` / `FarM`), each with a selection PERIOD (staggered by
    joint-table slot, capped by `RenderVKClusterRTRigMaxStale`) and an RT tolerance MULTIPLIER over
    the main view's (default 1 / 2 / 4 frames, x1 / x1 / x2). Near = the derived cut and
    shared welded skinning as before (caster = receiver); mid / far walk a coarser cut, own
    skinning (unwelded), self-shadow mismatch up to the multiplier x the raster error (the #747
    lift does not cover it).
  - DUE (`rigDue`, the same answer in modes 0 and 13): on the period, on arrival (not covered
    last frame), without a joint-table slot, and on forced frames (`RP_SCHED` bit 1: the RT origin
    moved = teleport / region crossing, an arena or the state re-created, a frame gap). A not-due
    instance COPIES ITS CUT FORWARD (`st[ri][5]`, page-checked like the static reuse; the derived
    cut's entries now carry their virtual page for it) and its avatar is KEPT (`RP_KEEP` bit 1):
    no headers, skinning or CLAS; the RT pose lags by at most the period. The signature drops
    the shared flag while the cadence is on (a copied-forward cut stops matching the raster set
    whenever the raster LOD moves).
  - REFRESH (mode 13 `RIGAV_KEEP` 2): a due mid / far avatar whose cut set and range did not
    change keeps its slots and headers; mode 14 only rewrites each slot's ClusterInstance and
    stamps it with bit 31; mode 9 skins it, compares with the positions its CLAS was built from
    and rebuilds it only past `RenderVKClusterRTRigPoseEps` x the cut's error at that distance or
    at `MaxStale` frames (per-slot build frame `rig_addr[cap * 4 + s]`). A skipped slot writes
    nothing: CLAS, arena vertices and header ref (what hits read) are only ever written together.
  - Stats: `| rig cadence RT cut N clusters vs raster M, selected S inst / reused R, avatars due D
    kept K refreshed F (slots compared C, pose-skipped P), CLAS rebuilt B, max stale X frames`;
    section counters `rt_clRigSel` / `rt_clRigReused` / `rt_clRigClasBuilt` / `rt_clRigPoseSkip` /
    `rt_clRigStaleMax`. Timers unchanged (`c_cl_rt_select`, `c_cl_rig_clas`).
- **RT bookkeeping + CLAS reuse (#1011, `clean_cluster_rt.comp` header; live A/Bs
  `RenderVKClusterRTRigRank` / `RenderVKClusterRTRigReuse`, both default 1).** Party measurement
  (87 avatars, mesh-shader tier): every rigged RT cut was already derived (`derived cuts 9011` of
  9011; no page walk) and the raster selection kernel was 0.28 ms for 411k tested / 117k cut
  clusters, but `c_cl_rt_select` was 2.4 ms and ~90k rigged CLAS (7.6M triangles) were rebuilt per
  frame. So the work went into the derived-cut plumbing and the rebuild volume, not into an
  incremental raster cut (the selection kernel is also the per-cluster cull + emission, which runs
  every frame anyway):
  - RANKED SHARED CUT (`RP_RANK`): the main view's selection reduces each shared instance's
    counted entries (count, the RT walk's hashes `clCutHashA / B`, triangles) into `CL_B_INST_CUT`
    and ranks them in the aux word (bits 12..26 + `CLA_RANKED`: page index, then cluster index --
    a function of the cut set alone). While the cut is complete (`rigRankOk`: main slice within its
    cap, 2a's map complete, no 2a overflow) mode 11 only marks an instance that has an entry without
    a skinned slot anyway (plain store into its gather word: no derived / shared cut for it -- the
    old gather's per-instance completeness) and mode 12 scatters by rank
    (no per-entry atomic): an unchanged cut lands in the same arena slots every frame. 2a overflow
    frames fall back to mode 11's gather + the atomic order. Mode 13 runs on the whole workgroup
    (`rigAdmitPar`, the same decisions row for row; the first fit on shared memory).
  - CLAS REUSE (`RP_REUSE`, `RP_KEEP` bit 2 = continuity): every DUE avatar whose cut set and range
    equal last frame's is refreshed (`RIGAV_KEEP` 2, not only #1008's tagged mid / far ones) and,
    with the ranked cut, its instances stay on the SHARED skinned cut (`RIGOF_SHARED` 2: skin once;
    before, a refresh forced own skinning). Mode 12 stamps `RIG_STAMP_REFRESH` on a slot that still
    holds its CLAS's cluster at the same entry (header ref + `k | tris << 16` equal, built before);
    mode 9 copies 2a's vertices, compares them with the arena's (what the CLAS was built from) and
    rebuilds only past the avatar's tolerance: self-shadowing avatars `RenderVKClusterRTRigReuseEps`
    (20 um default, max 1 mm -- float noise of the eye-relative skinning, the size of the raster-vs-RT
    mismatch that exists anyway; the shadow-ray origin offset is ~0.5 um per metre of camera
    distance, so a larger value could show as self-shadow on double-sided rigged faces); avatars
    tagged no-self-shadow `RenderVKClusterRTRigPoseEps` x the cut's error at their distance (capped
    at 0.1 m), at most `MaxStale` frames old. Bit-identical positions keep the CLAS at any age. A
    skipped slot writes nothing (CLAS, vertices, header ref stay one snapshot; the header's 2a slot
    and instance words are rewritten, hits never read them).
  - Stats: Cluster RT line `| rig rank on|off reuse on|off: avatars refreshed F, slots compared C
    kept K, CLAS rebuilt B | skin: shared-copied S own-skinned O slots | cpu plan avg/max ms`;
    counters `rt_clRigReuseCmp` / `rt_clRigReuseSkip` / `rt_clRigOwnSkin` (+ `CNT_RIG_*` 72..75).
  - PROFILE: GPU sub-marks `c_cl_rts_reset` (fills, CLAS scan), `c_cl_rts_gather` (mode 11),
    `c_cl_rts_cut` (mode 0), `c_cl_rts_admit` (13), `c_cl_rts_place` (14), `c_cl_rt_select` (now mode
    12 only), `c_cl_rig_skin` (mode 9) + `c_cl_rig_clas` (the build only); the CLOD gpu sub line adds
    `c_cl_rt_select_sum` / `c_cl_rig_sum` (the old two figures). CPU: the CLOD summary's `cpu tick`
    split `(ctl / stream / select / skin)`, the gather's sampled split `(split: intake, loop [keys,
    2a notes], tail)` every 64th frame, hitch sections `vk_clusterRigLists` / `vk_clusterCullMain` /
    `vk_clusterSkipPlan`.
- **Stats:** `rt_cl*` section counters (+ `rt_clRig*`), GPU sub-timers `c_cl_rt_select` /
  `c_cl_rig_clas` / `c_cl_clas_build` / `c_cl_blas_build` / `c_cl_t0_blas` / `c_cl_t0_rig` /
  `c_cl_ptlas`, a 10 s `Cluster RT:` line with a `rigged` section (eligible / ready / overflow /
  invalid, arena, CLAS or per-cut BLAS, `legacy rigged excluded N (fallback M)`). #748 perf pass
  adds: `static admit >= T (skipped N), reused N` (or `static off`), `demand N pages (room R)`
  on the CLAS part, `avatars I in / O out (want N slots[, cut by the budget])` on the rigged part,
  `lift probe on|off (N switched faces traced legacy)`; the RT line's budget shows `cluster RT N`.
  #748 static pass adds: `CLAS pages L live / C cap in K chunks`, `avg N CLAS / frame`, `cluster
  BLAS N / frame implicit (rigged)`, `static BLAS built B kept K previous P no-block F / frame (avg
  ...), heap X KiB live, Y KiB touched / Z MB (epoch, repacks, re-created[, leaked])`, `static RT
  flips N / s` (an override row back after a frame on the legacy BLAS, or newly admitted), `not
  ready N / frame`; GPU sub-timer `c_cl_sblas_build`; the 5 s `GPU sub ms/frame` line's `worst:`
  part carries every cluster stage's worst frame. 1b's stats: `static N [owned O, flips F / s]`
  (raster ownership owned <-> legacy per second).

**Secondary views + the shared rig cut + the GPU pick (#702 integration, 1b + renderer):**
- **Statics** (`RenderVKClusterStatic 2`): a re-rendered sun cascade and the mirror select their
  own cut (2c roles: own LOD camera, tolerance, error unit -- shadow texels for the ortho
  cascades) and DRAW it in their pass (`CLPASS_SHADOW` / `CLPASS_PROBE` = the capture pass,
  `LLClusterDrawIn::list` = `CLLIST_VIEW`). The view is selected BEFORE that pass's legacy cull,
  which gets the exclusion bitset (`CULL_MODE_CLUSTER`; `clean_cull.comp` skips owned statics in
  ANY pass whose push carries it) only when the view was selected and its pipelines exist this
  frame (`LLCleanClusterVK::viewDrawReady`, renderer `clusterStaticExclusion`): never half. Under
  the #567 static / dynamic split the cascade views skip moving casters
  (`LLClusterViewIn::skip_dyn`), which the dynamic overlay keeps drawing legacy. Projector tiles
  and probe faces keep their legacy statics (no per-tile views; GPU-scheduled probe cameras).
  #748 perf pass 2: probe faces no longer select a cluster view at all (it was selection-only:
  a full selection per CPU-scheduled face that nothing drew; `RenderVKClusterViews` bit 2 is
  reserved).
- **Rigged (switched avatars): ONE skinned cut.** Design choice: a skinned arena per view would
  be one cluster pre-skin per view (4 cascades + tiles + 6 faces + mirror); the legacy path
  already skins each avatar once for every pass. So the MAIN view selects, for every switched
  avatar some secondary pass draws (the renderer's per-pass avatar visibility, `setRigLists`:
  list 0 SHADOW = on screen / sun / projector casters minus no-shadow avatars, list 1 CAPTURE =
  on screen / mirror / probe candidates), its cut at the main LOD WITHOUT frustum / cone / Hi-Z
  culling; its own draw records stay culled. The one cluster pre-skin over the main slice skins
  all of it (the arena is sized for every DAG level, so un-culled fits by construction), and
  each such entry also appends a record to its rig lists. The shadow passes draw list 0, the
  captures list 1 (`CLLIST_RIG_*`, view = the main view); their legacy rigged draws then push
  `u_sky_zenith.w = 1` and drop the owned instances (`RC_CLUSTER`). Gate per pass and frame:
  `LLCleanClusterVK::rigListReady` (the list carried avatars, the pre-skin of THIS frame was
  recorded, the pass's pipelines exist), plus the setting bit and, for captures, a plain-lit
  frame (the ray-query lit FS uses set 2, the cluster set's index). Secondary views never select
  rigged instances. Caster = receiver: the shadow maps see exactly the surface the main view
  draws (the shadow-map acne on switched avatars). The shadow views' own LOD camera / texel
  tolerance therefore applies to statics only: a coarser caster cut for avatars would bring the
  receiver / caster mismatch back. Cost: the off-screen part of the visible / casting avatars is
  skinned too (never invisible avatars: the list bits follow the legacy passes' own visibility).
- **RT hook (2b):** the same shared cut is the RT input the switched avatars need: with an
  avatar's list bits set, its whole cut (main LOD, un-culled) is in the main slice, skinned in
  `SKIN_OUT` (map = `CLK_MAP_BASE` + selected index) after `recordPreskin`, and enumerated by the
  rig-list records (firstInstance = the entry). An RT consumer can add a third list (or reuse
  list 0) instead of building its own un-culled cut + arena. **Used (#748 rigged RT):** an
  instance complete in the shared cut takes its SKIN_OUT vertices (no list needed: RT finds the
  entries through the main slice + map); RT keeps its own cut / skinning only as the fallback
  (see 2b above).
- **GPU pick (#702):** `recordPickFilter` (compute, `clean_cluster_pick.comp`: the main view's
  drawn entries whose world sphere -- rigged: + posed margin -- the ray crosses inside the
  request's window → tier-0 records in `CL_B_PICK`) and `recordPickDraw` (inside the pick's 1x1
  scope: the vertex-pulling VS `-DCLEAN_PICK` + `clean_pick.frag -DPICK_OUT_SET=3`, layout
  {draw0, draw1, cluster, pick out}). `v_inst` = the renderer Instance row, so
  `LLCleanCursorPick` maps it to the object / TE exactly as a legacy fragment (a reported face
  must have a TE: `teFace()`). When it runs, the pick's legacy rigged layer drops `RC_CLUSTER`
  (`wv.sky_zenith.w = 1`). `pickNow` between frames uses the last LIVE frame's main view (its
  slot's set; arena, counters, map and `SKIN_OUT` still hold that frame). Snapshot recordings
  leave the pick's view alone. The pick costs no wait of its own: the `vk_performPick` hitches
  were `pickNow`'s blocking submission for a click's async pick (also with no cluster path);
  `RenderVKPickAsyncClick` (llcleancursorpick.h `holdClick`) now holds such a pick until the
  per-frame answer for its pixel is in (2-4 frames, input held with it).

**Rigged alpha-blend on the cluster path** (2026-10-14; 1b `vkgfx/llcleanclusterselectvk.cpp` "RIGGED
ALPHA-BLEND", `clean_cluster_alpha.comp`; replaces the legacy rigged alpha path under LOD on):
- **Eligibility:** `kRowNever` no longer holds the alpha bit (2): a skinned alpha-blend mesh row is
  cluster-routable like any other (no legacy geometry upload, skinning, BLAS; RT traces its
  cluster cut through `rowOnClusterPath`). Its ClusterInstance carries `CLI1B_BLEND` (8192).
- **Contract with LL:** LL orders alpha back to front BETWEEN faces (drawable distance, split at
  the water plane) and draws a face in its index order (no per-triangle sort). The cluster path
  keeps exactly that: instances back to front, each instance's clusters contiguous in a FIXED order.
- **Selection** (`clean_cluster_select.comp`, main view only): a blend instance is selected like
  any rigged one (its entries feed the pre-skin, the shared un-culled cut, the derived RT cut),
  but its draw records NEVER enter buckets 0 / 1 or the rig lists' buckets. Each record is STAGED
  with a tag {entry, staged list (0 main / 1 + L rig list L), local index}; the local index is
  the record's rank in (page, cluster) order -- deterministic for a given cut, so overlapping cards
  of one mesh never swap draw order frame to frame. One entry per instance (lid 0 allocates it):
  instance, posed box, record counts per list, main-list triangles; later its bases.
- **Order** (`recordAlphaOrder`, after the cluster pre-skin, same command buffer; GPU only):
  mode 0 PREP unions the instance's staged records' SKINNED cluster boxes (the posed position;
  rest-pose mesh centre when nothing was skinned); mode 1 ORDER (one 1024-lane workgroup) keys every
  entry exactly as `clean_alpha_sort.comp` mode 0 (far side of the water first, distance
  descending, k1 = instance index), bitonic-sorts the entries in global memory, and prefix-sums
  the counts in sorted order: main records split pre-water / post-water, rig list 0 (shadow
  casters), rig list 1 (captures: back to front over both sides, as the legacy captures drew the
  main sort's pre + post lists); mode 2 SCATTER moves each staged record to base + local index.
  Overflow (staging or entry table full) is DROPPED and counted, never appended unordered; both
  capacities follow the read-back demand (+25 %, shrink after ~5 s under a quarter, pow2;
  `kMinAlphaCap` 4096 .. `kMaxArena` records, `kMinAlphaInst` 1024 .. 65536 entries).
- **Layouts:** `CL_B_COUNTERS` views are 96 words (was 64): 42 `CLC_AL_STAGED`, 43 `_INST`,
  44 `_OVERFLOW`, 45 + k `_COUNT` (k = `CLA_OUT_PRE / POST / RIG0 / RIG1`), 49 `_DISPATCH` (3),
  52 + 4k `_TASKS`, 68 `_SORTED`, 69 / 70 main-view alpha instances / triangles (stats).
  `CL_B_DRAWS` after the culled rig list: the staging region + 4 output lists (alpha_cap records
  each), then words: tags (2 per staged record), the entry table (16 words per entry), the sort
  keys (4 words per entry). `ClSelectAux` is 208 B: + `alpha` {first alpha record, alpha_cap,
  entry cap, bit 0 on}.
- **Draws** (`LLClusterDrawIn::alpha` / `alpha_list`; `CLPASS_ALPHA`, `CLPASS_ALPHA_DEPTH`,
  `CLPASS_TRANS_DEPTH / _COLOR`): pass 2a / 2b draw the pre / post list INSIDE the legacy rigged
  alpha span (`g_p2_pre_rig` / `g_p2_post_rig` and their pipeline-statistics query now cover both
  paths) with the LIT_RIG_ALPHA template (blend, no depth test, react R|B, VRS) and the plain or
  ray-query blend FS (cluster set at 3), twice -- cull FRONT with push f8 = 1 (single-sided
  instances collapse in the MDI VS / launch no mesh group in the task stage), then BACK -- as the
  legacy `drawListBlend`. The alpha-blend depth pre-pass (`g_alpha_depth`) draws both lists with
  `clean_prepass_blend.frag`. Sun cascades: rig list 0's alpha list alpha-tested into the depth
  map (`!mShadowTransOn`, "Full" avatar shadows) or into the translucency layer (depth, then
  colour); projector tiles: alpha-tested ("Full"); mirror / CPU and GPU probe faces: rig list 1's
  alpha list blended after the static alpha (the mirror rigged-alpha template, plain FS). The
  mesh-tier task stage now compacts ORDER-PRESERVING (prefix over the kept flags) for every list
  -- the rasterization order of mesh-shader primitives follows the task / mesh workgroup order.
  The MDI tiers draw the alpha rig lists un-culled (no per-view rig-list cull for them).
- **Admission:** blend instances get `CLI1B_DRAW` only while the alpha lists can run
  (`alphaPipesReady`: order pipelines + pass 2's blend pipelines of the tier in force); before
  that they are not drawn (pop-in, never a legacy copy).
- **Tolerance:** `RenderVKClusterBlendTolMaxPx` (default 2): a blend instance's `tol_scale` is
  lowered so that tol_px x tol_scale <= the cap (relative to the main view's post-controller
  tolerance, `control().roleTolPx(CLROLE_MAIN)`), so GPU pressure (x8) never coarsens hair cards
  past 2 px; the RT cut follows (same instance scale). A pruned / released group renders only
  where its whole patch projects under the tolerance: at 2 px and 1440p a 5 cm card cluster
  keeps full detail inside ~30 m, a whole 0.4 m hair face reaches its ~4-triangle floor only past
  ~250 m (8 px: ~8 m / ~60 m).
- **Stats:** `CLOD main: ... | alpha cl faces F tris T (gathered, entries, recs pre / post /
  shadow / cap, overflow, caps) vs legacy faces L` (L = the main cull's rigged alpha draws, pre +
  post; what is still legacy). GPU: `c_cl_alpha_order`.
- **Shared-memory ORDER (#826):** with at most 1024 entries (every crowd short of a huge one) mode 1
  sorts the keys in shared memory (the prefix-sum array `s_sc`, reused): one lane per key and a
  workgroup barrier per bitonic step, then one write of the sorted keys -- the same keys, network
  and order as the global-memory path, which stays for more than 1024 entries (it paid a buffer
  barrier and a global read / write of every key per step: ~55 steps at 1024 entries, on one SM).

**2c → renderer / 1b** (#749, `vkgfx/llcleanclustercontrolvk.h`, owned by the core: `control()`):
- `LLClusterViewIn::role` / `sub` per view; the core's `recordCull` fills `tol_px` (role scale ×
  the knob) and `lod_cam` and holds `px_scale` for every role but `CLROLE_OTHER` (RT keeps its own).
- `ClusterInstance::tol_scale` = `control().avatarTolScale(...)` for avatars, at the marked line in
  1b's gatherer (static: 1); evaluated once per avatar (and self flag) per frame.
- Inputs: `LLClusterFrameIn::pressure` (the #613 GPU-time sample, frame target, VRAM ladder),
  `diag` (A/B line), `main_cam` / `main_px_scale` / `main_h` / `znear`, `snapshot`.

## 10. Order of operations per frame

All of this happens on the graphics queue, inside `LLCleanRendererVK::recordWorld`:

1. `buildRiggedFrame()`, the legacy rigged path, unchanged.
2. `beginTextureFrame()`: this frame slot's fence has been waited.
3. **`clusterTick(cmd, cullIn, height)`** (hitch section `vk_clusterTick`) → `LLCleanClusterVK::tick`
   (CPU only since #749; its cost is the CLOD line's "cpu tick"):
   1. Settings and tiers (`ForceTier` is live).
   2. Free retired buffers; harvest this slot's stats readback.
   3. The 2c controller (pressure, main view), then the CPU frame setup:
      - `stream().beginFrame()`: readback harvest, cache / builder poll, registration,
        liveness, eviction, upload plan. Skipped on a snapshot's second or later recording.
      - `select().beginFrame()`: gather instances (`acquireMesh`), upload `ClusterInstance[]`.
        The gather is INCREMENTAL (hitch section `vk_clusterGather`, CLOD line "cpu tick ...
        gather ..."): it walks a candidate list of the eligible rows, updated from the instance
        store's row change log (`LLCleanInstanceStore::setChangeLog` / `takeChangeLog`: the
        dirtied runs `copyDirtyVK` consumed in `syncFromStores`, i.e. exactly the rows the GPU
        mirror took; full rescan on the first frame, a store size change, a log overflow or a
        `RenderVKClusterStatic` on / off switch). Per row it caches the resolved key, the shared
        per-key record (stream directory hint; `groupBestSlot` while
        `LLCleanMeshAsset::bestSlotGeneration` is unchanged), the wearer + bind shape (rigged)
        and the xform / sigma_max / det (static: rebuilt on a row or render-offset change;
        rigged: when the wearer's position / rotation change). Per avatar per frame: one object
        lookup, the build priority and `avatarTolScale`; per key per frame: one acquire by index.
        Rigged rows' object / skin / wearer are re-checked by a rolling revalidation (32 rows per
        frame). The rows, order, flags and exclusion bits it produces are the old gather's.
        This always runs, because its per-slot buffers live in this slot. On a snapshot's
        second or later recording (`in.offscreen`) it re-uploads the cached list without
        re-gathering.
   4. Write this slot's descriptor set from the registered buffers.
   **`clusterFrameStart(cmd)`** → `recordFrameStart` (#749), at the start of the front-end section,
   into the command buffer the selection runs in: the front-end segment F under
   `RenderVKFramePipelining` (F waits only P(N−1), so these writes must be in F), else graphics
   (page copies then go to a forked async compute segment when async compute is on):
   5. Record: stats block → readback, then zero it.
   6. Record `stream().recordUploads()`, except on a snapshot's second or later recording:
      last frame's requests → readback, request reset, page copies, table / mesh / group
      updates, one barrier to the shader stages.
   7. Record `select().recordBegin()`: zero the counters.
   8. Barrier.
4. **`clusterSetRigLists` + `clusterCullMain()`** → `recordCull(view)` (legacy-skin skip: now
   BEFORE the legacy main cull, which the selection never read):
   1. the first `recordCull` of the frame (the main view's, after `recordFrameStart`, same
      command buffer) records 2a's `skin().recordBounds()`: the joint-stretch table and every
      rigged instance's posed-bound margin, before any selection;
   2. `select().recordSelect()`: selection, culling and emission for the main view
      (`SelectPush.rig_bounds`); it returns the cluster view index.
   3. **`clusterLegacySkipPlan()`** (CPU): the need mask per switched avatar (§11), skip bits into
      av_vis, the per-avatar words re-uploaded (`vkCmdUpdateBuffer`, barriered) for the cull.
   Then the legacy main cull (`dispatchCull`, with the exclusion bitset: owned static instances
   are skipped, owned rigged ones get `RC_CLUSTER`, and those of a skipped avatar are dropped).
4b. **The cluster pre-skin** (2a, #747), still in the front end: after the legacy palettes
   (`c_skin_pal`), the legacy pre-skin and the legacy weld (`recordRigWeld`), the renderer calls
   `skin().recordPreskin(cmd, mainView, prev, mark)`: WAR barrier, pose generations
   (`vkCmdUpdateBuffer`) [+ the meta fill after an arena re-creation] → ARGS (+ the frame's
   clears) → barrier → SWEEP + CHAR → barrier → ALLOC → barrier → [sampled legacy-verts stat] →
   pre-skin (skips reused slots; weld insert, one entry block per workgroup) → [weld on: barrier →
   weld → barrier → apply, both INDIRECT over the entries written] → final barrier (+ header
   readback copy). Weld off: 5 barriers, 5 dispatches (+1 sampled). GPU span
   `c_cl_preskin_begin` → `c_cluster_preskin` (cluster class 4 of the sampled rigged ring, out of
   `mRigGpuMs`), split by the sub-marks `c_cl_sk_prep`, `c_cl_sk_sweep` (+ CHAR),
   `c_cl_sk_alloc`, `c_cl_sk_legacy`, `c_cl_sk_skin` (incl. weld insert), `c_cl_sk_weld`,
   `c_cl_sk_apply` (each = time since the previous mark; class-4 links of the rigged ring).
   CLOD skin line: `slots live L kept K new N demand peak P cap C resets R; reuse on|off skinned
   S reused U chars held H skinned K; window disp D barriers B xfers X weld groups G` (skinned /
   reused / verts counted on every 16th frame). Its weld part ends with the #798 SEAM DIAGNOSTICS
   of a sampled frame: `seam rest|bind C cand paired P welded W noop O multi U unwelded nopair N
   weights M gap G norest R border-free F on A av; rest rows L/C peak K builds new n owner o shape s bones b noav x`
   -- the pairing space, the candidates PAIRED there (a partner within eps with matching weights,
   `CLK_WELD_PAIRED`) and WELDED, the NO-OPs (the blend equals the vertex: left alone,
   `CLK_WELD_NOOP`), those with more than one partner (`CLK_WELD_MULTI`), the unwelded ones by reason (no other mesh's vertex within eps in
   the pairing space / within eps but skin weights below `RenderVKRigWeldWeightMatch` / past
   `RenderVKRigWeldMaxGap` in this pose; header words `CLK_WELD_NOPAIR` .. `CLK_WELD_GREJ`), the
   boundary vertices not inserted for want of a rest row (`CLK_WELD_NOREST`), the candidates of
   clusters whose generating group is `CLG_FACE_BORDER_FREE` (their face's open border was
   simplified, so the partner mesh's vertices there weld to nothing: a sawtooth seam) with the
   number of avatars showing any (`CLK_WELD_BFREE` / `_AV`, avatar bits `CLW_AV_WORDS` words before
   the drop bits), then the REST TABLE (rows live / cap / peak; builds of the last ~30 s by trigger).
   **WELD PAIRING SPACE (#798, 2026-10-16): legacy mode 1's.** `RenderVKRigWeldMode` governs both
   welds: 1 (default) pairs every boundary vertex at its avatar's REST pose (bind pose skinned with
   the avatar's shape + joint overrides, identity rotations: `clean_rig_weld_common.glsl rwRestPos`,
   shared with `clean_rig_weld.comp`), 0 (A/B) in bind space.
   **WELD PAIR CACHE (#826, `RenderVKClusterWeldPairCache`, default on, live): pair once, apply per
   frame.** The pairs depend on the rest pose, the weights and the cut, not on the animated pose,
   and a cluster keeps its arena slot while it stays in the cut, so an ARENA VERTEX (slot x 64 +
   local) is a stable id for a weld candidate. Per row (character), decided row-uniformly in the
   pre-skin: CACHED when no cluster of it is new to the cut (ALLOC's new bits, which include a kept
   slot skinned at another pose generation), no host REPAIR bit (its rest row changed this frame;
   every row when eps / weight match / pairing space change or the cache was off), its last pairing
   is valid and found no candidate with more than one partner mesh, and no insert of it was dropped
   since. A cached row's candidates get weld entries (dst, avatar, instance, row | bit 31) but no
   rest-pose evaluation and no grid insert; the weld (mode 0) reads the candidate's PAIR word (its
   one partner's arena vertex), validates it (both slots assigned no later than the row's pair
   frame -- ALLOC stamps the slot's assignment frame on a miss -- and the partner in the cut this
   frame) and runs the shared resolve (`rwBlend*`) with it; an invalid partner = no weld to it this
   frame (the same as the search when the partner's cluster is not in the cut). A SEARCH row inserts
   and searches as before, and its entries store their pairs (one partner mesh: its chosen vertex;
   none: CL_NONE; more than one: CL_NONE + the row's multi frame -> the row keeps searching) and
   stamp the row's pair frame. A dropped insert (weld cap) stamps the multi frame: search again.
   Layout (SKIN_OUT, after the weld status; `clean_cluster_skin_common.glsl` WELD PAIR CACHE): PAIR
   (cap x 64 words), ALLOC (cap), ROW PAIR (2 per row: pair frame, multi frame; CL_NONE after an
   arena reset), REPAIR (33 words, host `vkCmdUpdateBuffer` when non-zero or after a non-zero
   frame). +260 B per arena cluster (2116 -> 2376 B). Stats (sampled frames): CLOD skin line
   `pairs cached C (invalid I) searched S` (scratch header `CLK_WELD_CACHED / _INVALID / _SEARCHED`).
   Known difference from the per-frame search: a stored partner that left the cut is not replaced
   by another candidate of the same mesh at the same rest position (a cluster-border copy) until the
   row re-pairs; such copies are bit-identical, so this only matters when the stored copy's cluster
   is culled while the other copy's is drawn (the seam vertex is then not welded for those frames). The rest rows come from a persistent per-avatar REST TABLE
   (2a's set 1 binding 5, `llcleanclusterskinvk.h` REST TABLE): rows for the admitted avatars,
   built by `LLRigRest::buildRow` (the legacy discovery's rows) on legacy mode 1's triggers (slot
   owner, skeleton serial ≥ 300 frames, bone rest hash ≥ 120 frames), ≤ 8 a frame, uploaded with
   `vkCmdUpdateBuffer` after the pre-skin's WAR barrier; capacity follows the live rows (steps of
   16, shrink with hysteresis, compacting); a slot whose row changed re-skins its character whole
   (new pose generation: reused slots hold the old welds). An avatar without a row yet inserts no
   candidate (never mixed spaces). CROSS-PATH seams (a cluster mesh against a legacy mesh of the
   same avatar) are NOT welded: each weld sees only its own path's vertices. In the cluster-owns-
   rigged mode the rigged rows that stay legacy are HUD (never welded), flexi, invisiprim,
   bounds-unknown (transient) and SYSTEM-AVATAR body parts (bit 17: weld candidates on legacy) --
   so the one real case is a system body part meeting a cluster mesh (e.g. a system head on a mesh
   body). Both welds now pair in the same space with the same rules; closing it means inserting
   the avatar's legacy work items into the cluster grid as partners and writing both streams
   (TODO).
   **THE RESOLVE (#798, 2026-10-16): legacy mode 1's, one copy** (`clean_rig_weld_common.glsl`
   `rwBlend*`, used by `clean_rig_weld.comp` GATHER and `clean_cluster_preskin_weld.comp`): ONE
   partner per other mesh -- its nearest candidate in the pairing space (equal distances: the best
   normal agreement; legacy's bucket order was arbitrary) -- and the symmetric blend (own + partners
   within the max gap; normals within the angle, a harder edge keeps its own). Was, on the cluster
   path: the centroid of every candidate of each other mesh within eps, normals summed over them.
   NO-OP: when the blend's result equals the vertex in this pose (within 50 um and 2 deg: a lattice
   step of two meshes' 16-bit positions, oct16 normals) nothing is written, on both paths -- seamless
   partners, a hard edge past the angle (own normal kept), or both: the weld closes gaps, it does not
   re-shade (re-quantise normals, re-orthogonalise tangents) an edge the creator made seamless;
   partially, that drew a line along such seams. (Gaps under ~0.1 mm / normal spreads under ~4 deg
   are left as they are.)
   Scratch header 40 -> 48 words (`CLK_INST_BASE` 48). `RenderVKClusterDebug 6`: cyan = every
   candidate of the cluster already seamless (WELD STATUS bits 28..31; 24..27 rejected).
   Seen on screen with `RenderVKClusterDebug 5` (red = border-free level, orange = the same as a
   fallback for a missing finer page). Under `RenderVKFramePipelining` all of 4 and 4b
   record in the front-end segment F; the pre-pass and G-buffer readers record after F's join.
5. Shadows, probes, mirror (#749 views, #702 integration draws): a re-rendered cascade / the
   mirror selects its cluster view BEFORE its legacy cull (exclusion when it draws), then draws
   its statics in its pass; the cascades (also cached ones, in the dynamic overlay), the dynamic
   projector tiles, the dynamic mirror and CPU-scheduled dynamic probe faces draw the shared rig
   list before their legacy rigged draws (which drop the owned avatars). Probe faces select no
   cluster view (#748 perf pass 2). MDI tiers: a per-view rig-list cull before each such scope. GPU pick (`LLCleanPickVK`, after the HUD): its select records the
   cluster pick filter, its draw rounds the cluster pick draw.
6. Depth pre-pass (#749): after the rigged pre-pass draws (`u_sky_zenith.w = 1`: switched avatars
   dropped), the **cluster pre-pass draw** (`CLPASS_PREPASS`, variant = the attachment set:
   depth / + AO normal / + motion), deferred frames only, P1 under two-phase occlusion. Rigged
   entries read the skinned arena (and, for motion, its previous positions) exactly as in 7.
7. G-buffer scope: static, then rigged, then **cluster draw** (`recordDraw`, `CLPASS_GBUFFER`).
   The renderer re-pushes the world draw view first.
7b. Ray tracing (`LLCleanRTVK::recordBuild`, on graphics after the front-end join, before the
   frame's P): the legacy BLAS plans, then 2b's `plan()` (CPU), `writeSets()`, and after the
   legacy BLAS builds `record()`: RT cut → CLAS builds → cluster BLAS (or tier-0 per-cut BLAS) →
   override rows; then TLAS pass A (binding 22 swaps the cluster BLAS in) and the TLAS (or PTLAS)
   build. Reads this frame's ClusterInstance list and page table (written by `recordFrameStart`
   / 1c, ordered by the join). Rigged cluster RT (#747 + #748): after the RT cut, mode 9 skins
   the switched avatars' cut from this frame's palettes (the legacy palette pass ran in the
   front end) and their CLAS (tiers 1 / 2) or per-cut BLAS build / refit (tier 0) follow; TLAS
   mode 4 (switched-group check) runs between the legacy rigged builds and pass B. Reads the
   main view's ClusterView (written at its `recordCull`) and 2a's margins (`recordBounds`).
8. `logMemory`: `mCluster->statsLine()` is appended to the VKMem line. Every 10 s the core logs
   one compact `CLOD` summary line, one `CLOD A/B old` line (#749), and one `CLOD main` line
   (`kFormatVersion` 3). The summary's stream part (`pages res … req … up … q …`) adds, only
   when non-zero, `fly F wait W (nosrc N busy B cap C) rebuild R` (reads in flight + loaded
   pages waiting for a slot; wanted pages not started: no source / cache read queue full or
   index pending / load cap; Live meshes rebuilding) and `STALL Ns` when pages were wanted and
   nothing was copied for N >= 2 s. The build part is `build qN bR/T X/s Yms wait W [regfull F]`:
   - queued jobs;
   - jobs in flight / worker threads;
   - finished builds per second and their mean worker ms (over about 5 s);
   - meshes waiting for their build source;
   - registrations refused last frame because the registry was full.

   Example of the main line:
   `CLOD main: faces 4446 (root 4100) tris 166043 cut 170112 of 847000 full (20%)`. That line
   covers the main view's instances drawn for real:
   - faces with a cluster on screen;
   - how many of them sit at the DAG floor (the whole cut is root clusters);
   - the triangles drawn;
   - their cut before culling;
   - their LOD0 triangles (`ClusterMesh::qheader[10]`).
   The percentage is cut / full, the LOD alone. The counters are `CLS_MAIN_*`, reduced per
   workgroup in `clean_cluster_select.comp` (faces / tris: what the main view DRAWS; the shared
   rig cut's off-screen entries are not counted there).
   #702 integration: a fourth short line, `CLOD views: st s123/4v r45 p0 | rig sh 6789 cap 2345
   (3/2 av, cap 8192) | drew sun 10/600 proj 0 cap 60/420 | pick 40` -- static draw records of
   the sun cascades (their view count), mirror and probe views; the shared rig lists' records
   (shadow, capture), the avatars in each and the records per list / bucket; the secondary
   cluster draws recorded in the last 10 s (sun static / rigged, projector tiles, captures static
   / rigged); the cluster pick filters run.
   **Fixed-cost audit (2026-10-14):** four more 10 s lines: `CLOD mem:` (total cluster VRAM =
   stream + skin + select + RT, then the stream's buffers `live/cap` MB: pool (peak, want, page
   fill %), staging, table, dir, groups, req, usage), `CLOD mem2:` (skin arena / scratch / stretch,
   select arena / draws / counters / instances / views / pick), `CLOD mem RT:` (the cluster RT
   pools) and `CLOD gpu sub` (the cluster sub-timers + the legacy stages they replace, 5 s
   avg/worst; needs `RenderVKGpuTimers`). The stream summary adds `fill F% (roots R%, P/S MB
   payload/slots) pool A/W MB`. Work no longer recorded when it has no input: secondary-view
   selections without a static drawn for real, the alpha order without a blend instance, the
   static half of the cluster RT (modes 1 / 2 / 3, the static CLAS build, modes 22 / 21, the static
   BLAS build) without a static in the cut; RT modes 11 / 12 dispatch from the main view's finalize
   args. The staging ring and the page pool follow their demand (first pool step 80 MB, granule an
   eighth of the want, slack an eighth of the resident set). The capture rig list is also culled
   against each face's near / far planes.
   **Event-driven upkeep audit (#826, 2026-10-22; measured on the 5090 4K serial: cluster upkeep
   ~5.7 ms against ~4.2 ms of raster saving).** Per stage, what already runs only on change, and
   what changed here:
   - `c_cl_sk_weld` (1.29): was a per-frame grid search for every candidate of every re-skinned
     row. Now the WELD PAIR CACHE (§10 4b): only rows with a cut / rest-row change search.
   - `c_cl_sk_apply` (0.60): over this frame's weld entries only (reused rows insert none, step 3);
     unchanged. Its entries now exist without the search.
   - `c_cl_alpha_order` (0.70): mode 1 sorted in global memory on one SM; now shared memory up to
     1024 entries (§9 rigged alpha-blend). PREP / SCATTER are per staged record (unchanged).
   - `c_cl_rt_select` (1.12): statics already reuse last epoch's cut while their reference camera
     holds (31 of 32 frames), switched avatars' cuts already derive from the shared skinned cut (no
     walk). Left: one workgroup per ClusterInstance including ineligible ones, and the walks of
     non-derived rigged instances (arena overflow, avatars outside every rig list).
   - `c_cl_rig_clas` (1.20) / `c_cl_blas_build` (0.58): an avatar is already KEPT (no CLAS, no
     copy) when its skinned WORLD positions are proven unchanged. Left (largest remaining item):
     rigged CLAS in CHARACTER space (2a's SKIN_OUT already is) with the character frame in the TLAS
     record (pass B in `clean_rt_instances.comp`), so a held pose rebuilds nothing while the avatar
     walks / turns -- the update-rate LOD's held frames would then cost no CLAS build at all.
   - `c_cluster_preskin` (0.81): held poses are reused (step 3); the fixed part is ARGS (clears of
     the key hash and avatar boxes), SWEEP (every arena slot) and ALLOC (every selected entry),
     O(arena) every frame even when no cut changed.
   - `c_blas_build` (0.45): legacy BLAS of statics not on the cluster path (§1 statics rule).
   Counters for the A/B: CLOD skin line (`skinned S reused U`, `pairs cached C (invalid I)
   searched S`), Cluster RT line (`reused`, `derived cuts`, `avatars kept K rebuilt R`).
9. A dropped frame (#697) calls `frameDropped()`: residency restarts (no page flips resident
   without its copy), the per-frame zero fills are redone.

**Latency:**
- Requests made in frame N are copied to the readback of frame N+1's slot and harvested
  `kFrameSlots` frames later.
- Stats work the same way, through the core readback.
- Nothing ever waits on the GPU.

## 11. Integration TODOs left in shared code

**2026-10-14: CLUSTER-OWNS-RIGGED (§1).** The legacy-skin skip below is now unconditional in that
mode: `clusterLegacySkipPlan` sets `kAvVisClSkip` on every avatar a pass draws; the need mask is
kept only as a HOLE COUNTER (`CLOD A/B old: ... keep <sun proj mir prb pick rt sel off>` = passes
that drew nothing of a drawn avatar this frame). RT is never a consumer (`rt_legacy_sw` false):
cluster-path rows are traced only from their cluster cut; with `RenderVKClusterRT 0` /
`RenderVKClusterRTRigged 0` (or the cluster RT inactive) they are not traced at all. The legacy
rigged capacity no longer adds the skipped rows (`R_CLSKIP_*`). Every RT caster is in rig list 0's
un-culled cut (records still follow the emit mask), so its RT cut DERIVES from the welded SKIN_OUT:
ray-traced seams match the raster's (#574); the RT arena's own skinning (no weld) remains for the
arena overflow and tier 0 only. HOLES with LOD on (nothing drawn there for cluster-path meshes):
the selection outline / wireframe, the RT terminator-triangle pass (#664), `queryNow` (rect
select, media focus, planar alignment), overdraw-stats (atomic) frames, passes whose `RenderVKClusterViewDraw`
bit is off, GPU-scheduled probe faces / RT-lit captures where the capture list does not draw. Our
own avatar while hidden in mouselook stays admitted (2026-10-14): the main cluster view carries
`CLV_HIDE_SELF` and draws / picks none of its `CLI_SELF` instances, while its cut still feeds the
shadow / capture rig lists and the cluster RT (it casts and is reflected, as legacy). RT plan
(2026-10-14): cluster-switched rigged rows enter the rigged RT plan WITHOUT a legacy slot
(`LLCleanRTVK::recordBuild`, slot kSentinel, geometry-less cluster-only entries) -- in this mode
their legacy slots do not exist; the `RT avatars:` line reports `cluster cut N of M with cluster
RT data ready`, and a lasting mismatch logs `RT avatars INCONSISTENT`. Alpha-blend rigged meshes are on the cluster path (ordered alpha lists, 2026-10-14). What stays LEGACY by design (and alone
sizes the legacy buffers): flexi, HUD, worn rigid attachments, system-avatar body
parts, invisiprims, statics (§1). VRAM: the 5 s `VRAM by path (cluster LOD on, owns rigged | on,
legacy rigged | off)` line -- legacy geom (packed, rigged share, routed slots not uploaded),
legacy rigged buffers, RT BLAS static / legacy rigged, cluster pages / skin / select / RT, totals.
The history below describes the pre-2026-10-14 model (switch per avatar, legacy kept as fallback).

**Legacy exclusion, TODO(1b), at `recordMainCull` / `clean_cull.comp`.** Instances the cluster
path draws must be skipped by the legacy main cull, or they are drawn twice.
- Planned mechanism: a module-owned bitset `cluster_owned[instance]`, a new cull-set binding, and
  a cull-mode bit that applies in the main pass only.
- Shadows and probes keep legacy drawing until 2c gives them cluster views. Verify that the
  shadow / probe rigged paths still get their pre-skin work items.
- **Rigged (2a, done):** whole avatars switch per `RenderVKClusterRigged`; the rest of this
  item is the wave-1 history (an avatar not switched still shows only as the debug overlay, in
  rest pose). Wave 1:
  - the cluster draw of rigged instances runs only with `RenderVKClusterDebug` ≠ 0, as an
    overlay with exclusion off;
  - selection, streaming and stats run for real whenever `RenderVKClusterLOD` is on.

**LEGACY-SKIN SKIP (done; `RenderVKClusterLegacySkinSkip`, default on, live).** Per switched
avatar (2a's admit) and frame, `LLCleanRendererVK::clusterLegacySkipPlan` builds the NEED MASK --
every consumer that would still read its legacy pre-skinned vertices -- after the main view's
selection and before the legacy main cull, with the passes' own gates:
- sun cascades / projector tiles (casters: main | sun / proj, not no-shadow): `RenderVKClusterViewDraw`
  bit 1 / 2 and `rigListWillDraw(CLPASS_SHADOW / PROJ)` (= `rigListReady` before the pre-skin is
  recorded: `select().rigDrawReady` carries every check of the rig-list draw,
  `skin().preskinWillRecord` the pre-skin's early-outs -- a covered pass always draws);
- mirror (main | mirror) / probe faces, CPU and GPU scheduled (main | probe-ok): bit 4 / 8 and
  `rigListWillDraw(CLPASS_PROBE)`. GPU jobs draw the capture list under a patched view (probe
  patch kind 3: clipped away when the GPU's job draws no avatars); ray-query lit frames use the
  captures' RT variant (cluster set at 3, `mCapRtPipe`; with RT, `passReady(PROBE)` needs both);
- GPU pick (main): bit 16 and `LLCleanPickVK::clusterPickArmed` (snapshot recordings: no pick);
- RT (`rtConsumerActive`, RT candidacy, not no-shadow): only when RT would build the avatar's
  LEGACY BLAS (`LLCleanRTVK::rigSwitchedLegacyAllowed`: the cluster RT does not trace switched
  avatars and the legacy avatar class is in RT). 2026-10-13: switched groups are cluster-only in
  RT from their first frame (no 8-frame `clusterRigCovered` wait -- with the legacy class dropped
  over budget RT never confirmed, and `keep rt` kept their legacy pre-skin running for nothing);
  RT dropped or not tracing = "not needed", not "not confirmed";
- the selection outline (a selected rigged face of the avatar, or one unresolved), wireframe;
- the setting off, no rigged frame, no main selection or no cluster pre-skin this frame.
An empty mask sets `kAvVisClSkip`: the main cull drops the owned rows (no draw, pre-skin, weld,
off-screen region); RT keeps their switched groups CLUSTER-ONLY (entries for pass B / mode 4,
no rig_pos range, rig copy or legacy BLAS; a kept BLAS ages out and is rebuilt if needed). Any
bit set: the legacy work runs exactly as before (never half). Transitions: the mask is per
frame, the pre-skin runs before every consumer, a skin-cache range re-skins in full, motion
vectors come from the previous-frame palettes (F6 history: previous = current that frame); the
legacy capacity stays sized as if the skipped rows were skinned (`R_CLSKIP_*` added to the
demand), so a switch back never drops draws. Remaining soft case: a cluster-only RT group whose
cluster data goes missing traces NOTHING for up to kFrameSlots + 1 frames (its flag re-arms the
mask) -- e.g. a covered avatar denied the rigged RT arena (mode 13; last frame's admitted avatars
go first, so only when the arena itself shrank under them or their own demand grew past it); tier 0 always keeps the legacy BLAS. Stats: `CLOD A/B old: skinned verts X skip N Vv av
S/W keep <sun proj mir prb pick rt sel off counts>` (the old line's verts are the work actually
done), `RT: ... rigged N groups / F faces (M MB, cluster-only K)`, 2a's `vs legacy verts`.

**Still legacy for switched avatars (the mask keeps them; history of the list below):**
- the drawn-geometry query (`queryNow`: rect select, media focus, planar alignment) reads the
  legacy snapshot: a skipped avatar's rigged faces are not in it (the debt statics already have);
- the RT terminator-triangle pass (#664) is no reader (it drops switched avatars) and has no
  cluster draw: the terminator fix-up is missing on switched avatars (needs a cluster variant of
  its pre-pass-shaped pipelines);
- the selection outline / wireframe keep the selected avatar / every avatar legacy;
- flexi / HUD / system-body rows are never cluster-eligible (alpha-blend rows are, since 2026-10-14).

**What still read a switched avatar's LEGACY pre-skinned vertices before the skip (#702
integration) -- kept for the history:**
- RT (#748 rigged RT, done): switched avatars trace their cluster cut. The legacy rigged BLAS
  stays BUILT as the fallback of a group whose cluster RT data is not ready that frame (and with
  `RenderVKClusterRTRigged 0`): its rig copy + build / refit still read the legacy pre-skin. To
  drop it, RT must accept "neither" for such a frame or guarantee readiness (tier 0 needs a
  harvest first). The RT terminator-triangle pass (#664) drops switched avatars and has no
  cluster draw.
- The legacy weld (`recordRigWeld`, #574 / #641): it runs over the legacy work list, owned
  instances included (pointless for them once nothing draws their legacy vertices).
- GPU-scheduled probe faces (#478 jobs: the GPU decides their camera / dynamic flag / avatar
  cull), every face of a ray-query lit frame, and the mirror in a ray-query lit frame.
- Passes whose `RenderVKClusterViewDraw` bit is off, or a frame whose rig list is not ready
  (no main cluster view, pre-skin not recorded, pipelines compiling): that pass draws legacy.
- The drawn-geometry query (`LLCleanPickVK::queryNow`: rect select, media focus, planar
  alignment) reads the legacy snapshot only: cluster-owned statics have no entry there.
- The selection outline (wireframe) of a switched avatar.
- (history) Alpha-blend rigged rows were never cluster-eligible: they always stayed legacy (their avatar can
  still switch); likewise flexi, HUD and system-avatar body parts.
- 2a's legacy stats pass (`CLK_LEGACY_*`) counts them on purpose.
So dropping the work items needs: the RT legacy fallback above, the weld skipping owned instances, the GPU
probe jobs drawing the capture list (a dynamic bit per job from the GPU), an RT-lit capture FS
with the cluster set at index 3, and the outline / query on clusters.

**Other TODO markers:**
- **TODO(1b) Hi-Z import:** `clusterTick` passes `hiz_view` / `hiz_sampler` when the Hi-Z is
  valid this frame.
- **Async compute queue (done, #749):** page copies through the renderer's fork (non-pipelined
  frames) or inside the front-end segment (pipelined); family facts (indirect copies,
  timestamps) per queue family; growth waits for forked copies to age kFrameSlots frames.
- **2a (done, #747):** the cluster pre-skin over the main view's slice of `CL_B_SELECTED`, posed
  bounds, the per-avatar switch, the weld (#574) on the cut, layering (#611). Integrated with 2c:
  the switch follows the cluster pre-pass, skinned clusters have motion vectors.
- **Gaps after 2a + 2c:**
  - Switched avatars' legacy pre-skin work: dropped per avatar and frame by the legacy-skin
    skip (above) whenever every consumer is on the cluster path.
  - Under `RenderVKPreskinPrevHistory` (F6) the legacy path builds no previous-frame palettes:
    skinned clusters get previous = current (camera-only motion) there.
  - The RT terminator-triangle pass (#664) drops switched avatars (`u_sky_zenith.w = 1`) and has
    no cluster draw yet.
  - RT of switched avatars: **done (#747 + #748, rigged cluster RT, §9 2b)** -- they trace the
    cluster cut the main view draws. FALLBACK (`RenderVKClusterRTRigged 0`, `RenderVKClusterRT
    0`, or a group whose cluster RT data is not ready that frame) keeps their legacy rigged BLAS
    (LOD geometry), whose mismatch with the receiver is what the receiver lift below covers:
    the receiver (the pre-pass depth: the cluster surface) lies up to the cut's error from that
    geometry; where it lies INSIDE the legacy hull every shadow ray hit the hull: hard-edged
    self-shadow patches along the crossing curves, moving with the pose (seen at 64 px). Lift
    (#747): pass B flags the switched faces traced on their LEGACY geometry that frame
    (`rt_geom.w` bit 31, `RT_GEOM_W_CLUSTER_RIG`, from the main cull's `RC_CLUSTER`; never set on
    a face traced on its cluster cut, so the lift applies only where the occluder is legacy); the RT
    shadow mask (`clean_rt_shadow_mask.comp`) probes along the camera ray within 2 eps of the
    receiver for such a face and then lifts the sun / projector ray origins by eps =
    `tol_px x max(distance, znear) / px_scale` (the main view's post-controller values,
    `LLCleanClusterVK::shadowLiftBound`, frame info `cluster_rt`) x
    `RenderVKClusterShadowLiftScale` along the light-facing normal (only where the visible
    side faces the light). Occluders nearer than the LOD's own error go unresolved there; at
    the default 1 px tolerance eps is one pixel footprint. Not covered: the shadow-map
    cascades / projector tiles (same mismatch, normal-offset bias only), the lit FS's inline
    traces (transparent receivers: drawn legacy anyway), and the legacy LOD slot's own
    deviation from the cluster LOD0 at distance (not bounded by eps). Cluster draws in the
    shadow views close the shadow-map side (#702 integration: done for the cascades / projector
    tiles -- caster = receiver there).
  - Rigged cluster RT leftovers: the RT cut is not welded (seam vertices differ from the raster's
    by at most `RenderVKRigWeldDistance`); visible clusters are skinned twice (2a's arena and the
    RT arena); the camera-ray probe of the lift still runs while any avatar is switched (finds
    nothing where the cluster cut casts); tier 0's LOD lags the raster cut by kFrameSlots frames
    + the build budget, and a slot found evicted on its BLAS's build frame keeps the instance
    legacy until its next rebuild (cut change or every 600 frames). STILL READING THE LEGACY
    SKINNED VERTICES of switched avatars after it (for dropping their legacy pre-skin): the legacy
    rigged BLAS (rig copy + build / refit: the RT fallback), the RT terminator-triangle pass, and
    the non-RT passes above.
  - A PTLAS session (`RenderVKClusterLOD` on at startup, `RenderVKClusterRT 2`) stays
    partitioned when cluster LOD is turned off at runtime (the PTLAS-only pipeline keeps
    building it; no cluster set needed).
  - The cluster pre-skin / selection spans are timed only on the graphics queue: under frame
    pipelining they read 0 / "-".
  - A deferred → forward switch leaves owned instances excluded for up to 2 frames (the gate
    follows the last G-buffer cluster draw).
- **2c (done, #749):** `znear`, the tolerance controller, temporal stability (lagged LOD camera,
  held px_scale, eased tolerance), selection views for cascades, probe faces, mirror and
  snapshots (snapshot resolution, base tolerance), the pre-pass draws (`CLPASS_PREPASS`).
- **#702 integration (done):** cluster draws in the sun cascades, projector tiles, mirror and
  CPU-scheduled dynamic probe faces (statics per view where a view exists, switched avatars from
  the shared rig cut), GPU pick of cluster-drawn objects. Not covered: probe-face / projector
  statics, GPU-scheduled probe faces, RT-lit captures (see the list above).
- **`CLPASS_FORWARD` (done, 2026-10-14):** the main view's records in the forward lit pass 1
  (`LLClusterRenderEnv::forwardDesc` = LIT_RIG's state; plain / ray-query variants), the cluster
  pre-pass on those frames too. Each snapshot logs "Cluster LOD snapshot WxH: lit path ..., cluster
  draw pass ..., instances drawn for real ..., main view selected / draw records" (a WARNING when
  owned rigged instances had no cluster draw).
- **TODO:** the RT terminator-triangle pass (#664) of cluster-drawn objects;
  snapshot recordings stream nothing (a photo draws what the live view made resident; finer pages
  it requests arrive after it). (Per-view culling of the shared rig lists: done, #748 perf pass 2
  rig-list cull.)

## 12. Settings

| Setting | Default | Notes |
|---|---|---|
| `RenderVKClusterLOD` | off | |
| `RenderVKClusterForceTier` | −1 | live; every change logs the tier line |
| `RenderVKClusterDeviceFeatures` | 1 | startup: with LOD off at startup, 0 no cluster extension, 1 the inert ones, 2 + mesh shaders (§7) |
| `RenderVKClusterTolerancePx` | 1.0 | 0.05 .. 1024 (large values force the DAG floor) |
| `RenderVKClusterProtectPx` | 0.5 | 0.05 .. 1024 |
| `RenderVKClusterRTTolerancePx` | 2.0 | 0.05 .. 1024 |
| `RenderVKClusterPagePoolMB` | 0 | 0 = auto |
| `RenderVKClusterUploadMBPerFrame` | 24 | |
| `RenderVKClusterCacheMB` | 2048 | |
| `RenderVKClusterBuildMsPerFrame` | 2.0 | render-thread ms per frame for the builder (intake + source copies); the workers run unmetered |
| `RenderVKClusterBuildThreads` | 0 | 0 = auto |
| `RenderVKClusterMeshGraceSec` | 30 | |
| `RenderVKClusterHysteresis` | 0.15 | LOD camera dead band = this × 2 m; px_scale coarsening margin |
| `RenderVKClusterViews` | 7 | #749: 1 cascades, 2 reserved (probe faces select no view since #748 perf pass 2), 4 mirror (cluster views of statics) |
| `RenderVKClusterViewDraw` | 31 | #702 integration: passes that DRAW the cluster path: 1 cascades, 2 projector tiles, 4 mirror, 8 probe faces, 16 GPU pick; 0 = main view only |
| `RenderVKClusterDebug` | 0 | |
| `RenderVKClusterRigged` | 1 | #747: 0 off, 1 when the cluster pre-pass exists, 2 forced |
| `RenderVKClusterSkinArenaMB` | 0 | #747: skinned arena cap; 2026-10-14: the arena follows the demand under it; #1010: 0 = auto (1/16 of the VRAM budget, 128 MB .. 4 GB), a crowd that does not fit coarsens whole avatars (least important first), never a partial avatar |
| `RenderVKClusterLayerErrorScale` | 2.0 | #747 / #611: error factor for layered avatars |
| `RenderVKClusterBlendTolMaxPx` | 2.0 | rigged alpha-blend on the cluster path: the largest pixel tolerance of their cut (0 = the opaque tolerance); live |
| `RenderVKClusterShadowLiftScale` | 1.0 | #747 fallback: RT shadow receiver lift on switched avatars whose occluder is still their legacy rigged BLAS that frame, x the cut's error bound (0 off, max 4); no effect where the cluster cut casts |
| `RenderVKClusterRT` | 1 | #748: 0 legacy BLAS, 1 cluster RT cut, 2 + partitioned TLAS (startup; experimental, 2026-10-14) |
| `RenderVKClusterPTLASPartition` | 1 | PTLAS partitions: 0 instance id order, 1 spatial grid (+ global); a change re-creates the PTLAS |
| `RenderVKClusterPTLASCellM` | 64 | spatial PTLAS grid cell (m, 8 .. 1024) |
| `RenderVKClusterRTHysteresis` | 0.25 | #748: reference-camera dead band (fraction of distance) |
| `RenderVKClusterRTBuildsPerFrame` | 8 | #748: tier-0 per-cut BLAS rebuilds per frame |
| `RenderVKClusterRTMemoryMB` | 0 | #748: 0 = a third of the RT budget, 64 MB .. 3 GB (static pass; was 1 GB), every cluster RT arena inside it; the rigged arena + CLAS first (from demand), the static CLAS arena from its demand in what is left |
| `RenderVKClusterRTStatic` | 1 | #748: statics drawn by the cluster path trace their cluster cut (the most important ones that fit); 0 = every static keeps its legacy BLAS in RT; live |
| `RenderVKClusterRTRigged` | 1 | #747 + #748: switched avatars trace their cluster cut (0 = their legacy rigged BLAS + the receiver lift); live |
| `RenderVKClusterRTStaticPersist` | on | #748 static pass: static cluster BLAS persist in a heap, rebuilt only when their cut / CLAS change (off = rebuilt every frame with the rigged ones, A/B); live |
| `RenderVKClusterRTStaticRasterTol` | 1 | static cut perf pass: static RT cut tolerance >= this x the raster's (base x 2^pressure level); 0 = the fixed RT tolerance; live |
| `RenderVKClusterRTStaticMB` | 0 | static cut perf pass: static CLAS arena budget (0 = a quarter of the cluster RT cap, 64 .. 512 MB); live |
| `RenderVKClusterRTRigCadence` | 0 | #1008: rigged RT cut + CLAS on a per-avatar schedule (bands, periods, coarser mid / far cuts, pose-delta CLAS rebuilds) for avatars tagged no-self-shadow (classifyNoSelf) only; 0 = A/B, every frame on the raster's cut; live |
| `RenderVKClusterRTRigNearM` / `FarM` | 8 / 24 | #1008: band edges (m; 10 % dead band); live |
| `RenderVKClusterRTRigPeriodNear` / `Mid` / `Far` | 1 / 2 / 4 | #1008: frames between RT cut selections per band (staggered; capped by MaxStale); live |
| `RenderVKClusterRTRigTolNear` / `Mid` / `Far` | 1 / 1 / 2 | #1008: RT cut tolerance x the raster's per band (1 = the raster's cut, derived + shared skinning; capped at 0.1 m past the raster error); 1 .. 64; live |
| `RenderVKClusterRTRigMaxStale` | 8 | #1008: most frames an avatar's RT geometry may lag (caps periods; pose-kept clusters rebuilt at this age); live |
| `RenderVKClusterRTRigPoseEps` | 0.5 | #1008: a refreshed cluster is rebuilt only when a vertex moved more than this x the cut's error there; 0 = rebuild all; live. #1011: also the no-self-shadow avatars' CLAS reuse tolerance |
| `RenderVKClusterRTRigRank` | 1 | #1011: the selection's per-instance cut summary + entry ranks feed the derived RT cut (no mode-11 gather, slots in rank order), mode 13 on the workgroup; 0 = A/B; live |
| `RenderVKClusterRTRigReuse` | 1 | #1011 (needs Rank): unchanged avatars keep slots + shared skinning, a cluster's CLAS is rebuilt only when it moved past the avatar's tolerance; 0 = A/B; live |
| `RenderVKClusterRTRigReuseEps` | 0.00002 | #1011: self-shadowing avatars' CLAS reuse tolerance (m; 0 .. 0.001; 0 = bit-identical only); live |
| `RenderVKClusterWeldPairCache` | on | #826: rows whose weld pairs stand re-apply them (no rest-pose evaluation, grid insert or search); off = every re-skinned row searches every frame (A/B); live |
| `RenderVKClusterLegacySkinSkip` | on | §11: no legacy pre-skin / weld / RT rig copy / legacy BLAS for a switched avatar nothing reads legacy this frame; off = A/B; live |

All are in `app_settings/settings.xml` with comments.

## 13. Build / check rules for wave 1

- Agents do not build.
- Syntax-check every touched `.cpp` with `g++ -fsyntax-only`, using that file's flags from
  `build.ninja`. Compile new GLSL with `glslc --target-env=vulkan1.3 -O`.
- A shader added to `SHADERS` means running `python3 indra/vkclean/regen_world_spv.py` and
  committing both. Stages are `comp / vert / frag / task / mesh`. Mesh shared memory is checked
  against 28 KiB and task against 32 KiB.
- Any change to a shared struct is made in both the `.h` and the `.glsl`, with `kFormatVersion`
  bumped. Report it; it is a core change.
