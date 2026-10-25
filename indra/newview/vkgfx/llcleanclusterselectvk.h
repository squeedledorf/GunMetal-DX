/**
 * @file llcleanclusterselectvk.h
 * @brief Cluster LOD (#642 / #745, OWNER: 1b): per-view GPU cut selection, culling and
 *        draw emission, and the cluster draws.
 *
 * Owns (allocates, registers with LLCleanClusterVK::setBuffer, retires):
 *   CL_B_INSTANCES  ClusterInstance[]   per frame slot, host-visible (VKMEM_SCENE); grows in
 *                                       power-of-two steps up to kMaxInstances
 *   CL_B_VIEWS      ClusterView[kMaxViews] + the 1b tail (ClSelectAux, see below), per frame
 *                                       slot, host-visible (VKMEM_CULL)
 *   CL_B_SELECTED   ClusterSelected[]   ONE arena (device), per-view slices sel_base / sel_cap;
 *                                       sized from the read-back demand (grow at once, shrink
 *                                       with hysteresis) between kMinArena and kMaxArena
 *   CL_B_COUNTERS   per-view counters (layout below) + the request dedupe stamps (device)
 *   CL_B_DRAWS      draw records (2 buckets x arena) + one aux word per arena entry (device,
 *                                       + INDIRECT usage)
 * Plus: a per-slot host-visible bitset for the legacy exclusion (read by the main cull through
 * its device address), a per-slot readback of the view counters (VKMEM_READBACK).
 * Writes (GPU) buffers 1c owns: CL_B_REQUESTS (append), CL_B_PAGE_USAGE (frame stamps).
 * Reads: CL_B_POOL, CL_B_PAGE_TABLE, CL_B_MESHES, CL_B_GROUPS, CL_B_JOINT_STRETCH, CL_B_HIZ.
 *
 * ---- LAYOUTS FOR CONSUMERS (2a pre-skin, 2b RT, 2c controller) -----------------------------
 * GLSL twin: indra/vkclean/shaders/clean_cluster_select_common.glsl.
 *   CL_B_COUNTERS, view v at word v * kViewWords:
 *     +0  kCntSelected   selected entries asked for (raw: may exceed sel_cap)
 *     +1  kCntDraws0     bucket 0 (single-sided) draw records written
 *     +2  kCntDraws1     bucket 1 (double-sided) draw records written
 *     +4  kCntDispatch   3 words {x, y, 1}: ceil(selected / 64) groups, rows of kGridX
 *     +8  kCntTasks0     VkDrawMeshTasksIndirectCommandEXT bucket 0 (32 clusters per group)
 *     +12 kCntTasks1     ... bucket 1
 *     +16 kCntSelClamped min(selected, sel_cap): the view slice's valid entries (USE THIS)
 *     +20 kCntRigDraws  (main view) 4 words: records of shared rig list r = list * 2 + bucket
 *     +24 kCntRigTasks  (main view) 4 x 4 words: the rig lists' mesh-tier task commands
 *   then from word kDedupBase: uint[kMaxVirtualPages] request dedupe stamps (1b only).
 *   Valid after recordSelect() for that view (its finalize ran; compute -> indirect / shader
 *   barrier recorded); consumers recorded later in the frame read them.
 *   CL_B_SELECTED[sel_base .. sel_base + kCntSelClamped): {cluster_ref, instance}.
 *   CL_B_DRAWS: view v bucket b records at [draw_base + b * draw_cap, + kCntDraws{b}) x 5 words
 *     (VkDrawIndexedIndirectCommand in tiers 1 / 2, VkDrawIndirectCommand + 1 word in tier 0;
 *     firstInstance = absolute selected index); aux word per selected entry at aux_base + sel:
 *     bits 0..7 DAG level, bit 8 fallback (finer page missing).
 *   Then (#702 integration) the SHARED RIG LISTS: rig index r's records at record rigBase + r x
 *     rigCap (rigBase = ceil((aux_base + arena) / 5)), firstInstance = the MAIN view's entry.
 *   CL_B_VIEWS tail at byte 4096 (after the kMaxViews ClusterViews): ClSelectAux {Hi-Z
 *     reference view, Hi-Z size, debug mode, aux_base, tier, rig {base, cap, on}, rig_bits}.
 *   ClusterInstance::flags bits 8..15 are 1b-private: 256 double-sided, 512 no cone (mirrored),
 *     1024 drawn for real (not only the debug overlay), 2048 avatar slot in bits 16..23 (rigged),
 *     4096 a moving shadow caster (#567 split).
 *   CL_B_PICK (binding 14, #702): the GPU pick's cluster list (word 0 count, records from word 4).
 *
 * ---- WHAT IS GATHERED (beginFrame, render thread) -------------------------------------------
 * The instance store's live rows, every frame (budgeted key resolution, cached per row). The
 * gather is INCREMENTAL: it keeps a candidate list of the eligible rows (updated from the
 * store's row change log, LLCleanInstanceStore::takeChangeLog) with each row's resolved key,
 * wearer, bind shape and cached xform, so a frame walks the candidates only -- no hashing,
 * object lookups or LOD-slot queries for unchanged rows (see "INCREMENTAL GATHER" in the .cpp):
 *   rigged mesh faces (flag 0 + skin): always while RenderVKClusterLOD is on. Their
 *     ClusterInstance::xform is the REST pose (avatar world x bind shape); 2a (#747,
 *     llcleanclusterskinvk.h) skins the cut, bounds the pose (margins) and decides per AVATAR
 *     which ones are drawn for real (CLI1B_DRAW + exclusion; the others stay legacy and show
 *     only in the RenderVKClusterDebug overlay, in rest pose);
 *   static mesh faces: with RenderVKClusterStatic >= 1 (1: selection / streaming / stats +
 *     the debug overlay; 2: EXPERIMENTAL real draw -- the G-buffer scope draws them and the
 *     legacy MAIN cull skips them, see legacyExclusion; needs the deferred G-buffer).
 *   Never: HUD, worn rigid attachments, terrain, water, invisiprims, flexi, trees / grass, GPU
 *     spinners, system-avatar body parts, bounds-unknown rows. Rigged ALPHA-BLEND rows are
 *     gathered (kInstBlend): drawn only from the ORDERED ALPHA LISTS (recordAlphaOrder; pass 2,
 *     the alpha-blend depth pre-pass, shadow / translucency / capture alpha lists), never in the
 *     opaque buckets.
 * An instance whose mesh 1c has not registered yet (acquireMesh kNone) is NOT DRAWN in the
 * cluster-owns-rigged mode (2026-10-14: the cluster path replaces the legacy one; every eligible
 * rigged row is in the exclusion bitset from its first frame, drawable or not, and its legacy
 * geometry is never uploaded -- rowClusterRoutable); outside that mode (statics, or rigged with
 * the mode off) it stays on the legacy path.
 *
 * ---- EMISSION TIERS (LLCleanClusterCaps RASTER, RenderVKClusterForceTier live) -------------
 *   2 MESH      task (32 records -> compacted payload) + mesh (one 64-invocation workgroup per
 *               cluster) via vkCmdDrawMeshTasksIndirectEXT on the finalize's task args;
 *   1 MDI_U8    vkCmdDrawIndexedIndirectCount, the pool bound as vertex buffer (stride 20) +
 *               VK_INDEX_TYPE_UINT8 index buffer, clean_draw.vert -DCLEAN_CLUSTER;
 *   0 MDI_PULL  vkCmdDrawIndirectCount, the VS pulls index + vertex from the pool words
 *               (-DCLEAN_CLUSTER_PULL; core Vulkan, every supported GPU).
 * Pipelines for every tier the device has are compiled at module init (asynchronously);
 * a tier whose pipelines are missing falls to the next lower one.
 * CMDGEN DGC (VK_EXT_device_generated_commands): not used yet -- see the TODO(1b) in the .cpp
 * for the plan (one generated sequence per view replacing the per-bucket draws + binds).
 *
 * ---- SECONDARY VIEWS + THE SHARED RIG CUT (#702 integration; CLUSTER_LOD_CONTRACT.md §9) ----------
 *   Static instances drawn for real are SELECTED PER VIEW (re-rendered sun cascades, the mirror:
 *   their own LOD camera / tolerance, 2c roles) and drawn there (CLPASS_SHADOW / CLPASS_PROBE =
 *   the capture pass), the pass's legacy cull skipping them through the exclusion bitset --
 *   only when that view's selection and pipelines exist this frame ("never half").
 *   Rigged instances (whole avatars 2a switched) are SKINNED ONCE: the main view selects the
 *   un-culled cut of every avatar some secondary pass draws (setRigLists: the renderer's
 *   per-pass avatar visibility), the cluster pre-skin covers it, and its entries also go to
 *   two shared rig lists (CL_RIG_LIST_SHADOW: cascades + projector tiles; _CAPTURE: mirror +
 *   dynamic probe faces) that those passes draw (LLClusterDrawIn::list) while their legacy
 *   rigged draws drop the owned instances (u_sky_zenith.w = 1). Secondary views never select
 *   rigged instances. RT (2b) can read the same cut: the main slice + map + SKIN_OUT.
 *   GPU PICK: recordPickFilter (the main view's drawn entries the ray crosses -> CL_B_PICK) and
 *   recordPickDraw (tier-0 pull VS -DCLEAN_PICK + clean_pick.frag at set 3).
 *
 * Wave 1 scope: the main view in the deferred G-buffer scope (CLPASS_GBUFFER). #749 (2c) adds
 * the main view's DEPTH PRE-PASS (CLPASS_PREPASS: depth only / + AO normal / + motion vectors
 * and the no-history mask, the renderer's three pre-pass attachment sets, with the mask FS
 * twins -- clusters have no opaque / mask bucket split -- so AO, the RT shadow mask, the Hi-Z
 * and TAA see cluster-drawn objects), and selection-only views for the sun cascades, probe
 * faces and the mirror (CLROLE_*: their selected lists feed 2a / 2b; their passes keep the
 * legacy draws -- drawing clusters there needs the exclusion in those culls, TODO).
 * TODO: CLPASS_FORWARD (no deferred G-buffer: the cluster path does not draw), the RT
 * terminator-triangle pass (#664) and GPU pick (#702) of cluster-drawn objects.
 *
 * ARENA (#749): each view's slice is sized from its ROLE's read-back demand (largest view of
 * the role last time, +25 % + 1024); the main view (recorded first) leaves room for the other
 * roles' views of the last frame, later views take their want or what is left.
 */
#pragma once

#include "vk_device.h"
#include "vk_pipeline.h"   // PipelineJobSlot
#include "../llcleanclusterformat.h"
#include "../llcleanclustercache.h"
#include "../llavatarslots.h"

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class LLCleanClusterVK;
struct LLClusterFrameIn;
struct LLClusterViewIn;
struct LLClusterDrawIn;
struct PipelineJob;
enum LLClusterDrawPass : uint32_t;

class LLCleanClusterSelectVK
{
public:
    explicit LLCleanClusterSelectVK(LLCleanClusterVK& owner) : mOwner(owner) {}
    ~LLCleanClusterSelectVK() = default;

    // ---- shared layout constants (= clean_cluster_select_common.glsl) ----
    static constexpr uint32_t kViewWords     = 96;   // (rigged alpha lists: was 64)
    static constexpr uint32_t kCntSelected   = 0;
    static constexpr uint32_t kCntDraws0     = 1;
    static constexpr uint32_t kCntDraws1     = 2;
    static constexpr uint32_t kCntDispatch   = 4;
    static constexpr uint32_t kCntTasks0     = 8;
    static constexpr uint32_t kCntTasks1     = 12;
    static constexpr uint32_t kCntSelClamped = 16;
    static constexpr uint32_t kCntRigDraws   = 20;   // + r (main view)
    static constexpr uint32_t kCntRigTasks   = 24;   // + r * 4 (main view)
    static constexpr uint32_t kCntRigCulled  = 40;   // + bucket (main view): the MDI per-view rig-list cull
    static constexpr uint32_t kRigLists      = 2;    // CL_RIG_LIST_SHADOW, CL_RIG_LIST_CAPTURE
    // RIGGED ALPHA LISTS (CLI1B_BLEND; clean_cluster_select_common.glsl CLC_AL_*, main view).
    static constexpr uint32_t kCntAlStaged   = 42;
    static constexpr uint32_t kCntAlInst     = 43;
    static constexpr uint32_t kCntAlOverflow = 44;
    static constexpr uint32_t kCntAlCount    = 45;   // + k (kAlOut*)
    static constexpr uint32_t kCntAlDispatch = 49;
    static constexpr uint32_t kCntAlTasks    = 52;   // + k * 4
    static constexpr uint32_t kCntAlSorted   = 68;
    static constexpr uint32_t kCntAlMainInst = 69;
    static constexpr uint32_t kCntAlMainTris = 70;
    static constexpr uint32_t kAlOutPre = 0, kAlOutPost = 1, kAlOutRig0 = 2, kAlOutRig1 = 3, kAlOuts = 4;
    static constexpr uint32_t kAlEntryWords  = 16;
    static constexpr uint32_t kMinAlphaCap   = 4096;   // staged / per-list alpha records
    static constexpr uint32_t kMinAlphaInst  = 1024;   // alpha entry table (pow2)
    static constexpr uint32_t kMaxAlphaInst  = 1u << 16;
    static constexpr uint32_t kPickCap       = 4096; // CL_B_PICK records
    static constexpr uint32_t kDedupBase     = LLClusterFmt::kMaxViews * kViewWords;
    static constexpr uint32_t kDrawWords     = 5;
    static constexpr uint32_t kGridX         = 32768;   // dispatch / task grid row width
    static constexpr uint32_t kTaskClusters  = 32;
    static constexpr uint32_t kViewsTailOffset = LLClusterFmt::kMaxViews * 256;   // ClSelectAux
    static constexpr uint32_t kSelectAuxBytes = 400;   // (+ the alpha lists' uvec4; rig bits 2 x 32 words at 1024 slots)
    // 1b-private ClusterInstance::flags bits.
    static constexpr uint32_t kInstDoubleSided = 1u << 8;
    static constexpr uint32_t kInstNoCone      = 1u << 9;
    static constexpr uint32_t kInstDraw        = 1u << 10;
    static constexpr uint32_t kInstAvatar      = 1u << 11;   // bits 16..23 = joint-table slot
    static constexpr uint32_t kInstDynCaster   = 1u << 12;
    static constexpr uint32_t kInstBlend       = 1u << 13;   // rigged alpha-blend: the alpha lists only
    static constexpr uint32_t kInstAvShift     = 16;   // 10 bits (16..25): kAvatarSlotMask
    // Bounds (no unbounded growth).
    static constexpr uint32_t kMaxInstances = 1u << 17;   // ClusterInstance rows per frame
    // #1010: ClusterInstance::tol_scale bound (protection < 1; the skinned arena's per-avatar
    // coarsening up to 2^8 > 1: llcleanclustercontrolvk.h setArenaPressure)
    static constexpr float kMaxTolScale = 256.f;
    static constexpr uint32_t kMinArena     = 1u << 15;   // selected entries
    static constexpr uint32_t kMaxArena     = 1u << 21;

    // Pipelines for every tier the caps allow (all of them, so RenderVKClusterForceTier can
    // switch live), buffers, registration. False = selection stays off (logged).
    bool init();
    void destroy();                      // device idle

    // Frame setup (CPU): gather the cluster-drawn instances (stream().acquireMesh per
    // instance-face), fill + upload ClusterInstance[] into THIS slot's buffer, reset the
    // view list. in.offscreen (a snapshot's 2nd+ recording): do NOT re-gather -- upload the
    // cached CPU list of the first recording into this slot (the snapshot holds still).
    void beginFrame(const LLClusterFrameIn& in);
    // Recorded after 1c's uploads: zero this frame's view counters (WAR vs last frame's reads).
    void recordBegin(VkCommandBuffer cmd);
    // One view: selection + culling + emission dispatches. Returns the view index or kNone.
    uint32_t recordSelect(VkCommandBuffer cmd, const LLClusterViewIn& view);
    // The view's draws in the current rendering scope (CLPASS_GBUFFER; CLPASS_PREPASS with
    // in.pre_variant = the renderer's pre-pass attachment set, #749; CLPASS_SHADOW / PROJ /
    // PROBE: a secondary view's own records (list 0) or the main view's shared rig list
    // in.list - 1). Returns true when draws were recorded.
    bool recordDraw(const LLClusterDrawIn& in);
    // #748 perf pass, MDI tiers: the per-view RIG-LIST CULL of rig list `list` against the draw
    // view at `draw_view_addr` (device address of the renderer's param-ring entry), recorded
    // OUTSIDE a rendering scope right before the view's scope; that view's recordDraw then passes
    // LLClusterDrawIn::rig_culled. False = not recorded (mesh tier: the task stage culls).
    bool recordRigCull(VkCommandBuffer cmd, uint32_t list, uint64_t draw_view_addr);
    // Pipelines of a pass exist for the raster tier in force.
    bool passReady(LLClusterDrawPass pass) const;
    // SHARED RIG CUT: the per-avatar pass bits (rig list L: bits[L][slot / 32] bit slot % 32)
    // the next main-view selection uses; on = false: no sharing (the plain culled main cut).
    // emit (#748 perf pass): bit L = rig list L's records are emitted (a pass draws it this frame);
    // a list without the bit still gets its avatars' un-culled cut (shared skinning, RT) but no
    // records, and rigListShared(L) is false.
    void setRigLists(const uint32_t bits[2][kAvatarSlotWords], bool on, uint32_t emit = 3u);
    // The main view of this frame selected the shared rig cut (rig lists exist this frame);
    // per list: that list carried its avatars (a list nobody could draw stays empty).
    bool rigShared() const { return mRigFrame == mFrame && mMainView != kNoView; }
    bool rigListShared(uint32_t list) const { return rigShared() && list < kRigLists && mRigListOn[list]; }
    // The main view's shared rig list of `pass` (shadow / projector: list 0, captures: list 1)
    // can be drawn this frame: EVERY condition recordDraw checks for that draw except the
    // pre-skin (the list carried avatars, the main view's records, the pass's pipelines, the
    // page pool). The legacy-skin skip commits on it before the passes record.
    bool rigDrawReady(LLClusterDrawPass pass) const;
    uint32_t mainView() const { return mMainView; }
    // CPU-side tally of a secondary draw (the per-view CLOD summary).
    void noteViewDraw(uint32_t pass_kind) { if (pass_kind < 8) ++mPassDraws[pass_kind]; }
    // GPU PICK (#702): pipelines (lazily, against the pick's output set layout) + the list
    // buffer + a main view this frame (recordPickFilter then records).
    bool pickReady(VkDescriptorSetLayout out_dsl);
    // Compute, outside a rendering scope: the main view's drawn entries the ray crosses inside
    // [t0, t1) -> CL_B_PICK. False = nothing to pick (no main view, no pipelines / buffer).
    bool recordPickFilter(VkCommandBuffer cmd, const float org[3], const float dir[3], float t0, float t1, float cone);
    // Inside the pick's 1x1 scope: the list. push = the pick's 16 B draw push (view, f8, entry,
    // mode). round 1 = the pick's count-then-fill retry: the count the window pass armed.
    void recordPickDraw(VkCommandBuffer cmd, const VkDescriptorSet draw_sets[2], VkDescriptorSet out_set,
                        const uint32_t push[4], uint32_t round);
    // Device address of CL_B_PICK (the pick's window pass arms round 2's count there).
    uint64_t pickListAddress() const { return mPickList.address; }
    // #749 (#697): the recording of the last frame start was dropped: the counters' first
    // whole zero fill (dedupe stamps) and the view readbacks of that slot may never have run.
    void frameDropped();
    // #749: per-role selected entries of the last read-back frame, compact ("m 1234 s 456 ...").
    void roleSummary(std::string& out) const;
    // #702 integration: the per-view line (static records per role, shared rig lists, the
    // secondary draws and picks of the last 10 s); resets the window counters.
    void viewSummary(std::string& out);
    // Pre-pass pipelines exist for this tier in force (the renderer draws clusters there).
    bool prepassReady() const;
    // CLPASS_FORWARD pipelines exist for the tier in force: the plain variant (rt false) or the
    // ray-query one (rt true: the frame's lit FS traces -- LLClusterDrawIn::rt_set).
    bool forwardReady(bool rt) const;
    // Snapshot diagnostics: the main view's draw records (both buckets) and selected entries of
    // the LAST recording of this slot, read from its host-visible counters readback (valid once
    // that recording's fence was waited; the harvest of the slot's next round still consumes it).
    bool peekMainCounts(uint32_t& draws, uint32_t& selected) const;
    // ---- RIGGED ALPHA-BLEND on the cluster path (CLI1B_BLEND) ----
    // The alpha lists exist (order pipelines + pass 2's blend pipelines for the tier in force):
    // blend instances are admitted only then (no legacy copy: never drawn opaque instead).
    bool alphaPipesReady() const;
    // After the cluster pre-skin (the posed boxes), in the same command buffer: the alpha ORDER
    // of this frame (clean_cluster_alpha.comp prep / order / scatter) over the main view's staged
    // records. cam = the camera (world / agent space), water = the water height, under = the
    // camera below it (LL's pre / post-water split). False = nothing recorded (no main view, no
    // alpha lists): the alpha lists stay empty (counts zeroed at the frame start).
    bool recordAlphaOrder(VkCommandBuffer cmd, uint32_t main_view, const float cam[3], float water, bool under);
    // The alpha lists of this frame can be drawn in `pass` (the order ran, the pipelines exist).
    bool alphaDrawReady(LLClusterDrawPass pass) const;
    // CLOD stats: "alpha cl faces F tris T (entries E, overflow O, cap C/I)".
    void alphaSummary(std::string& out) const;

    static constexpr uint32_t kNoView = 0xFFFFFFFFu;
    uint32_t instanceCount() const { return mInstanceCount; }
    // Static instances drawn for real this frame (kInstDraw, not rigged; RenderVKClusterStatic 2).
    // 0 = no consumer of anything static: secondary-view selections (they select statics only) and
    // the static half of the cluster RT (CLAS pages, static BLAS) have nothing to do this frame.
    uint32_t staticDrawn() const { return mStaticOwnedN; }
    // #1011 RT CUT SUMMARY (clean_cluster_select_common.glsl): this frame's main view writes each
    // shared instance's skinned-cut summary (CL_B_INST_CUT) and the entry ranks (aux words) --
    // RenderVKClusterRTRigRank on and the buffer allocated (2b's ranked shared cut reads them).
    bool rtSummaryOn() const { return mRtSummary; }
    // This frame's ClusterInstance rows as uploaded (CPU copy; the flags carry kInstDraw):
    // #748's rigged cluster RT keys the rigged TLAS groups of the switched avatars from it.
    const std::vector<LLClusterFmt::ClusterInstance>& cpuInstances() const { return mCpuInst; }
    // Instance-store row `row` belongs to the cluster path (a candidate row of the gather whose key
    // did not fail to resolve -- e.g. not a mesh -- or one queued for the candidate list), drawn
    // by it or not yet (loading / building). RT (LLCleanRTVK rigged plan, cluster LOD on) traces
    // such a rigged row only from its cluster cut once switched, never from legacy geometry; rows
    // outside the cluster path (flexi, system body, non-mesh) stay legacy.
    // (Hook for the "cluster LOD replaces legacy" work: refine eligibility here.)
    // 2026-10-14: in the cluster-owns-rigged mode every candidate rigged row is on the cluster
    // path (its legacy geometry is never uploaded), a key that failed to resolve too.
    bool rowOnClusterPath(uint32_t row) const
    {
        if (row >= mRowCand.size()) return false;
        const uint32_t i = mRowCand[row];
        if (i == kPendingAdd) return true;
        return i < mCands.size() && (mCands[i].state != 2 || (mOwnsRigged && mCands[i].rigged));
    }
    // The ROW-CONTENT rule of a rigged row on the cluster path (= the gather's rigged candidate
    // test): a live, skinned mesh row that is not HUD, a worn rigid attachment, terrain, water,
    // an invisiprim, flexi, a tree / grass, a system-avatar body part or bounds-unknown
    // (alpha-blend rows ARE routable: the ordered alpha lists draw them). The renderer's geometry fill uses it (no module needed, any thread) to leave such
    // rows' legacy GPU geometry out in the cluster-owns-rigged mode (2026-10-14).
    // (2026-10-14, rigged alpha on the cluster path: alpha-blend (2) is no longer a "never" bit --
    // a blend row is cluster-routable, drawn from the ordered alpha lists, CLI1B_BLEND)
    static constexpr uint32_t kRowNever = 8u | 512u | 32u | 2048u | 65536u | 524288u | 32768u | 131072u | 128u;
    static bool rowClusterRoutable(uint32_t flags, uint32_t skin_id, uint32_t lod_group)
    {
        return lod_group != 0xFFFFFFFFu && (flags & kRowNever) == 0u && (flags & 1u) != 0u && skin_id != 0xFFFFFFFFu;
    }
    // This frame runs in the cluster-owns-rigged mode (LLClusterFrameIn::owns_rigged + 2a works).
    bool ownsRigged() const { return mOwnsRigged; }
    uint32_t viewCount() const { return mViewCount; }
    // A view's slice of the selected arena (2a / 2b consumers).
    struct ViewSlice { uint32_t sel_base = 0, sel_cap = 0; uint32_t flags = 0; };
    ViewSlice viewSlice(uint32_t view) const;

    // LEGACY EXCLUSION (renderer recordMainCull, TODO(1b)): the bitset of the instances the
    // cluster path draws for real in the main view this frame (bit i = instance i), as the
    // device address of this slot's host-visible copy + its length in words. False = nothing
    // excluded (the main cull runs without CULL_MODE_CLUSTER). Static (RenderVKClusterStatic 2)
    // and the rigged instances of the avatars 2a switched over (#747; the main cull keeps those
    // in its buckets for the other views and marks them RC_CLUSTER for the main-view passes).
    bool legacyExclusion(uint64_t& address, uint32_t& words) const;
    // Selected entries the arena holds (2a sizes its per-entry map from it).
    uint32_t arenaEntries() const { return mArena; }
    // GPU timer scope names (renderer gpuSubMark) for the selection / draw of the raster tier
    // in force: "c_cluster_t<N>" / "g_gbuf_cluster_t<N>".
    const char* selectMarkName() const;
    const char* drawMarkName() const;

    void statsLine(std::string& out) const;
    uint64_t heldBytes() const;
    // Fixed-cost audit (2026-10-14, the 10 s "CLOD mem" line): per buffer "name live/cap" MB
    // (live = what the demand uses, cap = what is allocated), compact.
    void memSummary(std::string& out) const;

    // The CPU gather's cost since the last call (the 10 s CLOD line): "gather <avg>/<max> ms
    // walk <candidates / frame> chg <rows visited / frame> res <key resolutions / frame>";
    // resets the window.
    void gatherSummary(std::string& out);

private:
    // ---- INCREMENTAL GATHER (llcleanclusterselectvk.cpp, above gather()) ----
    static constexpr uint32_t kNoIdx = 0xFFFFFFFFu;
    static constexpr uint32_t kPendingAdd = 0xFFFFFFFEu;   // mRowCand: queued for the candidate list
    struct Cand                       // one cluster-eligible instance-store row
    {
        uint32_t row = kNoIdx;              // instance-store row id
        uint32_t row_gen = 0;               // the row's release generation (another one = another key)
        uint32_t lod_group = kNoIdx;        // row snapshot (a change re-resolves the key)
        uint32_t flags = 0;                 // row snapshot
        uint32_t skin_id = kNoIdx;          // row snapshot (rigged)
        uint32_t retry = 0;                 // frame after which a failed resolve may retry
        uint32_t mesh = kNoIdx;             // mMeshUse index (state 1)
        uint32_t avatar = kNoIdx;           // mAvatarUse index (rigged: the wearer)
        uint32_t bind = kNoIdx;             // mBinds index (rigged: the bind shape matrix)
        uint32_t pose = 0;                  // rigged: mAvatarUse[avatar].pose the xform was built from
        uint8_t  state = 0;                 // 0 unknown, 1 resolved, 2 not eligible / failed
        bool     rigged = false;
        bool     rec = false;               // xform .. neg_det below are valid
        bool     dead = false;              // removal pending (applyMembership)
        bool     finite = false;
        bool     neg_det = false;
        float    lod_scale = 0.f;           // sigmaMax3 of xform's 3x3
        float    size = 0.f;                // static: largest column length (build priority)
        float    xform[12] = {};            // the ClusterInstance xform (static: from the row)
        LLUUID   object;                    // owning object (state 1)
    };
    struct MeshUse                    // one resolved cluster key (shared by its rows)
    {
        LLClusterKey key;
        uint32_t refs = 0;
        uint32_t dir = kNoIdx;              // stream directory hint (acquireMesh)
        uint32_t src_slot = kNoIdx;         // groupBestSlot, valid while src_gen is current
        uint32_t src_gen = 0;               // LLCleanMeshAsset::bestSlotGeneration of src_slot (0 none)
        uint64_t frame = ~0ull;             // frame of the last acquire (once per key per frame)
        uint32_t ref_mesh = kNoIdx, ref_gen = 0;   // that acquire's MeshRef
    };
    struct AvatarUse                  // one wearer (shared by its rigged rows)
    {
        LLUUID   id;
        uint32_t refs = 0;
        uint64_t frame = ~0ull;             // frame of the values below (once per avatar per frame)
        bool     ok = false;                // found + alive this frame
        bool     posed = false;             // pos / quat / rm hold a pose
        uint32_t pose = 0;                  // bumped when the position / rotation change
        float    pos[3] = {};               // getPositionAgent
        float    quat[4] = {};              // getRotation
        float    rm[9] = {};                // LLMatrix3(rotation).mMatrix
        float    height = 0.f;              // mBodySize.mV[VZ]
        float    importance = 0.f;          // build priority (not self)
        float    tol[2] = {1.f, 1.f};       // avatarTolScale [self]
        float    arena[2] = {1.f, 1.f};     // #1010: its arena term [self] (avatarTolScale arena_out)
        uint64_t tol_frame[2] = {~0ull, ~0ull};
    };
    struct ViewRec { ViewSlice slice; uint32_t draw_base = 0, draw_cap = 0; bool valid = false; };

    void gather(uint64_t frame);
    // incremental gather helpers (rows = the store's Instance array as bytes)
    void intake(const uint8_t* rows, uint32_t n, bool full, const float d[3], uint32_t& visited);
    void visitRow(const uint8_t* rows, uint32_t id, const float d[3]);
    void applyMembership(const uint8_t* rows, const float d[3]);
    void markRemove(uint32_t idx);
    void buildStaticRec(Cand& c, const uint8_t* row, const float d[3]);
    bool resolveKey(Cand& c, uint32_t frame32);
    void resolveRig(Cand& c);
    void releaseMesh(Cand& c);
    void releaseRig(Cand& c);
    AvatarUse* avatarPose(uint32_t idx, uint64_t frame, const float cam[3]);
    void revalidate(uint32_t budget);
    void dropCandidates();
    void harvest(uint32_t slot);
    bool ensureInstances(uint32_t slot, uint32_t instances);
    bool ensureInstCut(uint32_t instances);   // #1011: CL_B_INST_CUT for this many rows
    bool ensureOwned(uint32_t slot, uint32_t bitset_words);
    bool ensureArena(uint32_t entries, uint32_t rig_cap, uint32_t alpha_cap = 0, uint32_t alpha_inst = 0);
    bool ensurePickList();
    uint32_t rigBaseRecords() const { return (mArena * 2 * kDrawWords + mArena + kDrawWords - 1) / kDrawWords; }
    // #748 perf pass: the MDI per-view culled rig list (2 buckets x mRigCap records) after the lists.
    uint32_t rigCulledRecords() const { return rigBaseRecords() + kRigLists * 2 * mRigCap; }
    // Rigged alpha lists: the staging region, then kAlOuts output lists (mAlphaCap records each),
    // then words: tags (2 per staged record), the entry table, the sort keys.
    uint32_t alphaBaseRecords() const { return rigCulledRecords() + 2 * mRigCap; }
    static VkDeviceSize drawsBytes(uint32_t arena, uint32_t rig_cap, uint32_t alpha_cap, uint32_t alpha_inst);
    bool ensureCounters();
    void createDrawPipelines();
    bool recordAlphaDraw(const LLClusterDrawIn& in);   // recordDraw's alpha-list path
    void pollPipelines();
    int32_t drawTier() const;
    void barrier(VkCommandBuffer cmd, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds,
                 VkAccessFlags2 da) const;

    LLCleanClusterVK& mOwner;
    uint32_t mInstanceCount = 0;
    uint32_t mViewCount = 0;
    uint32_t mSlot = 0;
    uint64_t mFrame = 0;
    bool mReady = false;
    bool mMesh = false;               // the device has the mesh-shader stages (layouts / barriers)

    // Pipelines. Compute: selection + finalize ({cluster set}, push 16 B). Draw: [tier][fs]
    // [bucket]; fs 0 = the material G-buffer FS, 1 = the debug-view FS; bucket 0 cull back,
    // 1 cull none. Tiers 0 / 1 use mDrawLayout ({draw0, draw1, cluster}, push VS|FS 16 B, so
    // the renderer's sets 0/1 + pushed view stay valid); tier 2 mMeshLayout (push TASK|MESH|FS).
    VkPipelineLayout mCompLayout = VK_NULL_HANDLE;
    VkPipeline mSelectPipe = VK_NULL_HANDLE, mFinalizePipe = VK_NULL_HANDLE;
    VkPipelineLayout mDrawLayout = VK_NULL_HANDLE, mMeshLayout = VK_NULL_HANDLE;
    VkPipeline mDrawPipe[3][2][2] = {};
    VkPipeline mPrePipe[3][4][2] = {};   // #749 pre-pass: [tier][variant: depth / +normal / +normal +motion / +motion only][bucket]
    // Secondary passes (#702 integration): [0 sun cascades, 1 projector tiles, 2 capture
    // (mirror / probe faces)][tier][bucket]. Shadow FS = the shadow alpha test, capture = the
    // plain lit FS (the ray-query variant's set 2 is the cluster set's here: RT-lit frames keep
    // the legacy capture draws).
    VkPipeline mPassPipe[3][3][2] = {};
    // Legacy-skin skip: the captures' RAY-QUERY lit variant [tier][bucket] (layout {draw0, draw1,
    // RT set, cluster at 3}; the FS kWorldFragRtSpv), used when LLClusterDrawIn::rt_set is set.
    // With an RT set layout, the capture pass is ready only when both variants exist.
    VkPipelineLayout mDrawLayoutRt = VK_NULL_HANDLE, mMeshLayoutRt = VK_NULL_HANDLE;
    VkPipeline mCapRtPipe[3][2] = {};
    // CLPASS_FORWARD (snapshot fix): the main view's forward lit pass 1 [plain / ray-query][tier]
    // [bucket] against env.forwardDesc (plain: mDrawLayout / mMeshLayout + kWorldFragSpv;
    // ray-query: mDrawLayoutRt / mMeshLayoutRt, the RT cluster stages + kWorldFragRtSpv).
    VkPipeline mFwdPipe[2][3][2] = {};
    // RIGGED ALPHA-BLEND (the alpha lists): pass 2's blend [tier][plain / ray-query lit], the
    // alpha-blend depth pre-pass [tier], the sun cascades' translucency layer [tier][depth /
    // colour], the captures' blend [tier] (plain FS, as the legacy mMirrorRigAlphaPipe); the order
    // compute (prep + scatter, and the one-workgroup order variant).
    VkPipeline mAlphaPipe[3][2] = {};
    VkPipeline mAlphaDepthPipe[3] = {};
    VkPipeline mTransPipe[3][2] = {};
    VkPipeline mCapAlphaPipe[3] = {};
    // DEPTH-ONLY VS twins (RenderVKDepthOnlyVS, live): the depth passes above rebuilt with the
    // cluster stages' depth flavour (kWorldCluster{VertPull,VertU8,Mesh}DepthSpv: gl_Position +
    // the alpha test's inputs; same desc otherwise): the pre-pass depth variant [tier][bucket],
    // sun / projector [0 sun, 1 proj][tier][bucket], the alpha-blend depth [tier] and the
    // translucency layer [tier][depth / colour]. Picked per draw by dvOr() while mDepthVs; a
    // missing twin (compile failure) falls back to the full-VS pipeline. The pre-pass twins
    // [tier][variant][bucket]: variant 0 the depth flavour, 1 / 2 / 3 the slim pre-pass flavours
    // (kWorldCluster*{Normal,NormalMotion,Motion}Spv: only what the normal / motion FS reads).
    VkPipeline mPreDvPipe[3][4][2] = {};
    VkPipeline mPassDvPipe[2][3][2] = {};
    VkPipeline mAlphaDepthDvPipe[3] = {};
    VkPipeline mTransDvPipe[3][2] = {};
    bool mDepthVs = true;             // this frame's RenderVKDepthOnlyVS (beginFrame)
    VkPipeline dvOr(VkPipeline dv, VkPipeline full) const { return mDepthVs && dv ? dv : full; }
    VkPipelineLayout mAlphaCompLayout = VK_NULL_HANDLE;
    VkPipeline mAlphaPassPipe = VK_NULL_HANDLE, mAlphaOrderPipe = VK_NULL_HANDLE;
    uint64_t mAlphaOrderFrame = ~0ull;    // frame whose order was recorded
    uint32_t mAlphaCap = 0;               // staged / per-list alpha records
    uint32_t mAlphaInst = 0;              // alpha entry table capacity (pow2)
    bool     mAlphaOn = false;            // this frame's alpha lists are on (cl_aux.alpha.w)
    // read back (main view): staged demand, entries, overflow, drawn instances / triangles, the
    // lists' records (pre, post, rig 0, rig 1)
    uint32_t mAlStaged = 0, mAlEntries = 0, mAlOverflow = 0, mAlMainInst = 0, mAlMainTris = 0, mAlCounts[4] = {};
    uint32_t mAlGathered = 0;             // blend instances gathered (CPU)
    // GPU pick: filter compute (cluster set + 48 B push) and the tier-0 draw.
    VkPipelineLayout mPickCompLayout = VK_NULL_HANDLE, mPickLayout = VK_NULL_HANDLE;
    VkPipeline mPickFilterPipe = VK_NULL_HANDLE, mPickDrawPipe = VK_NULL_HANDLE;
    PipelineJobSlot mPickJob;                       // mPickDrawPipe's background build (pickReady)
    VkPipeline mPickStage = VK_NULL_HANDLE;         // its output, installed when it lands
    // #748 perf pass: the MDI tiers' per-view rig-list cull (cluster set + 32 B push).
    VkPipelineLayout mRigCullLayout = VK_NULL_HANDLE;
    VkPipeline mRigCullPipe = VK_NULL_HANDLE;
    uint64_t mRigCullFrame = ~0ull;   // mFrame of the last recordRigCull (its region holds that view's list)
    uint32_t mRigCullList = 0;
    uint32_t mRigCulls = 0;           // recorded since the last viewSummary
    VkDescriptorSetLayout mPickOutDsl = VK_NULL_HANDLE;   // the out-set layout mPickLayout was made with
    bool mPickFailed = false;
    PipelineJob* mPipeJob = nullptr;
    bool mPipesReady = false;
#ifdef VK_EXT_mesh_shader
    PFN_vkCmdDrawMeshTasksIndirectEXT mDrawMeshTasksIndirect = nullptr;
#endif

    // Buffers.
    GpuBuffer mInst[3];               // per slot ClusterInstance rows (host-visible)
    uint32_t  mInstCap[3] = {};
    GpuBuffer mViews[3];              // per slot views + 1b tail (host-visible)
    GpuBuffer mOwned[3];              // per slot exclusion bitset (host-visible, device address)
    uint32_t  mOwnedWords[3] = {};
    GpuBuffer mReadback[3];           // per slot view counters (host-visible)
    bool      mReadbackValid[3] = {};
    GpuBuffer mSelected, mDraws, mCounters;
    GpuBuffer mPickList;              // CL_B_PICK (device; + INDIRECT)
    GpuBuffer mInstCut;               // #1011 CL_B_INST_CUT (device): the main view's per-instance cut summary
    uint32_t  mInstCutCap = 0;        // rows it holds (pow2 steps, <= kMaxInstances)
    bool      mRtSummary = false;     // this frame's main view writes it (rtSummaryOn)
    uint32_t  mArena = 0;             // selected entries the arena holds
    uint32_t  mRigCap = 0;            // records per shared rig list and bucket (CL_B_DRAWS tail)
    uint32_t  mArenaUsed = 0;         // allocated to views this frame
    bool      mCountersFresh = true;  // whole buffer (dedupe stamps) needs its first zero fill
    uint64_t  mPeakBytes = 0;

    // CPU frame state.
    std::vector<LLClusterFmt::ClusterInstance> mCpuInst; // this frame's rows (kept for offscreen)
    std::vector<uint32_t> mCpuOwned;                     // exclusion bitset (static, CLI1B_DRAW)
    bool mOwnedAny = false;
    // Incremental gather: the candidate rows (pool + free list), their order (row, pool index)
    // sorted by row, the row -> pool index map (kNoIdx / kPendingAdd), the shared keys and
    // wearers (refcounted, freed at 0), the rigged rows' bind shapes. All bounded by the store's
    // rows; released with the module.
    std::vector<Cand> mCands;
    std::vector<uint32_t> mCandFree;
    std::vector<std::pair<uint32_t, uint32_t>> mOrder;
    std::vector<uint32_t> mRowCand;
    std::vector<MeshUse> mMeshUse;
    std::vector<uint32_t> mMeshUseFree;
    std::unordered_map<LLClusterKey, uint32_t, LLClusterKeyHash> mMeshUseIdx;
    std::vector<AvatarUse> mAvatarUse;
    std::vector<uint32_t> mAvatarUseFree;
    std::unordered_map<LLUUID, uint32_t> mAvatarUseIdx;
    std::vector<std::array<float, 16>> mBinds;
    std::vector<uint32_t> mBindFree;
    std::vector<std::pair<uint32_t, uint32_t>> mRuns;   // (scratch) the store's change log
    std::vector<uint32_t> mAdds, mRemoved;               // (scratch) membership changes
    uint32_t mRows = 0;               // store rows at the last intake (a change = full rescan)
    const uint32_t* mRowGens = nullptr;   // (this gather) LLCleanInstanceStore::rowGenerations
    float    mRowD[3] = {};           // store render offset at the last intake (statics are agent space)
    bool     mStaticCands = false;    // statics are candidates (RenderVKClusterStatic != 0)
    bool     mChangeLogOn = false;    // the store's change log is on for us
    uint32_t mRevalCursor = 0;        // rolling rigged-row revalidation (mOrder position)
    // Gather cost (the 10 s CLOD line): ms, rows visited by the intake, candidates walked.
    double   mGatherMsAcc = 0.0, mGatherMsMax = 0.0;
    uint32_t mGatherFrames = 0, mGatherFull = 0;
    uint32_t mGatherCapped = 0;       // #1010 audit: frames the gather stopped at kMaxInstances (rows past it not drawn)
    uint64_t mGatherVisitAcc = 0, mGatherWalkAcc = 0, mGatherResAcc = 0;
    // #1011 PROFILE: every kGatherSampleEvery-th gather is split by phase (ms summed over the
    // sampled frames of the log window): intake (change log + revalidation), the candidate loop,
    // inside it the per-key mesh blocks (asset slot + stream acquire) and 2a's noteRow / noteInstance,
    // and the tail (2a admit + ownership). The inner timers add their own clock reads.
    static constexpr uint32_t kGatherSampleEvery = 64;
    double   mGsIntake = 0.0, mGsLoop = 0.0, mGsKeys = 0.0, mGsNote = 0.0, mGsTail = 0.0;
    uint32_t mGsFrames = 0;
    ViewRec mViewRecs[LLClusterFmt::kMaxViews];
    uint32_t mDebug = 0, mStatic = 0;
    bool mOcclusion = true;
    // Hi-Z reference: the main view recorded last frame (the Hi-Z built after it pairs with it).
    float mOccViewProj[16] = {};
    float mOccOrigin[3] = {};
    uint64_t mOccFrame = ~0ull;
    bool mHizOk = false;              // this frame: a pyramid of last frame + its view
    uint32_t mHizSize[2] = {};
    uint32_t mHizMips = 0;
    uint64_t mLastGbufDraw = 0;       // frame of the last G-buffer cluster draw (exclusion gate)
    bool mExclude = false;            // this frame's exclusion bitset (draw path proven)
    bool mExcludeStatic = false;      // ... used for static instances (RenderVKClusterStatic 2)
    bool mOwnsRigged = false;         // 2026-10-14: the cluster-owns-rigged mode this frame (LLClusterFrameIn)
    std::vector<uint32_t> mAdmitted;  // 2a: instance indices switched to the cluster path
    // Shared rig cut (#702 integration).
    uint32_t mRigBits[2][kAvatarSlotWords] = {};   // next main selection's per-avatar list bits
    bool     mRigWant = false;
    bool     mRigListOn[2] = {};      // per list: avatars in it and its records emitted (the last setRigLists)
    uint32_t mRigEmit = 3u;           // setRigLists' emit mask (ClSelectAux rig.w)
    uint64_t mRigFrame = ~0ull;       // frame whose main view selected the shared cut
    uint32_t mMainView = kNoView;     // this frame's main view (kNoView = none yet)
    uint32_t mRigDemand = 0;          // largest raw rig-list count (last read-back frame)
    uint32_t mRigLast[4] = {};        // rig list records written (last read-back frame)
    uint32_t mRigAvatars[2] = {};     // avatars in each list (last setRigLists)
    // Per-view CLOD summary (#702): static records per role, secondary draws recorded.
    uint32_t mRoleDraws[8] = {};
    uint32_t mPassDraws[8] = {};      // this 10 s window: passes that drew clusters (by kind)
    uint32_t mPassDrawsLast[8] = {};
    uint32_t mPickRuns = 0, mPickRunsLast = 0;
    uint64_t mPickFrame = ~0ull;      // frame / slot / view of the last LIVE main selection (pickNow)
    uint32_t mPickSlot = 0, mPickView = kNoView;
    uint64_t mPickListFrame = 0;      // frame CL_B_PICK was registered (every slot's set has it kFrameSlots later)
    bool     mSnapshotFrame = false;  // this tick records a snapshot (its main view is not the live one)

    // Stats (CPU side + read-back view counters).
    uint32_t mGathered[2] = {};       // rigged, static rows gathered
    uint32_t mPending = 0;            // eligible rows whose mesh is not registered yet
    uint32_t mResolvedThisFrame = 0;
    // #748 static pass: static rows drawn for real (RenderVKClusterStatic 2) and their ownership
    // flips (owned <-> legacy) per second over ~10 s windows (statsLine)
    std::vector<uint32_t> mStaticOwned, mStaticOwnedPrev;   // bit per instance-store row
    uint32_t mStaticOwnedN = 0;
    uint64_t mStaticFlipAcc = 0;
    double   mStaticFlipStart = 0.0, mStaticFlipsPerSec = 0.0;
    uint32_t mDemand = 0, mDemandPeak = 0, mLastSel = 0, mLastDraws[2] = {};
    // #749 per-role arena sizing: the role of each view recorded per slot, the views per role
    // and the largest per-view demand of each role (last read-back frame).
    uint8_t  mViewRole[3][LLClusterFmt::kMaxViews] = {};
    uint32_t mViewsRecorded[3] = {};
    uint32_t mRoleDemand[8] = {};
    uint32_t mRoleViews[8] = {};
    uint32_t mRoleSel[8] = {};
    uint64_t mShrinkSince = 0;
    int32_t  mLastTier = -1;
};
