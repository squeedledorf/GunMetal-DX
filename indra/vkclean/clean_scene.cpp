// vkclean — native Vulkan clean-renderer prototype using the REAL clean
// pipeline data layouts.
//
// This is NOT a generic cube demo. Every GPU struct here is byte-identical to
// the viewer's clean-pipeline stores, verified by static_assert against the
// documented sizes:
//   * Instance   (192 B) == LLCleanInstanceStore::Instance
//   * SlotInfo   ( 32 B) == LLCleanMeshAsset::SlotInfo
//   * LodGroup   ( 16 B) == LLCleanMeshAsset::LodGroup
//   * Material   ( 96 B) == LLCleanMaterialStore::Material
//
// The pipeline it drives is the real one:
//   persistent Instance SSBO  ->  compute cull (mirror of cleanCullC.glsl:
//   8-corner world AABB, p-vertex frustum test, GPU LOD select via
//   detailFromTan(lod_factor*radius/dist), residency gating on SENTINEL /
//   index_count==0, OOB guard, HUD split, per-bucket atomic emit)  ->
//   VkDrawIndexedIndirectCommands in 6 buckets  ->  vkCmdDrawIndexedIndirectCount
//   per bucket  ->  bindless per-material textures (descriptor-indexed array,
//   the Vulkan-native analog of the GL Material's GLuint64 handles).
//
// Verification is threefold: (1) a CPU reference cull that reproduces the shader
// exactly, checked per-bucket against the GPU counters; (2) a captured PNG; (3)
// validation + sync-validation (shader-access heuristic ON) must be clean.
#include "clean_scene.h"
#include "vk_pipeline.h"

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_FORCE_RADIANS
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

namespace {

const glm::vec4 kClear(0.12f, 0.16f, 0.22f, 1.0f);
constexpr uint32_t NUM_BUCKETS = 6;   // opaque, mask, alpha, r_opaque, r_mask, r_alpha
constexpr uint32_t NUM_LOD     = 4;
constexpr uint32_t LOD_SENTINEL= 0xFFFFFFFFu;

// ===========================================================================
// REAL data layouts. Field order / sizes must equal the viewer's C++ structs.
// ===========================================================================

// LLCleanInstanceStore::Instance — 192 B.
struct Instance {
    float model_matrix[16];   //  64
    float aabb_min[4];        //  16  (.w = LL LOD radius)
    float aabb_max[4];        //  16  (.w = attach bone; unused in static proto)
    float tint[4];            //  16
    float uv_matrix[16];      //  64
    uint32_t lod_group_id;    //   4
    uint32_t material_id;     //   4
    uint32_t flags;           //   4  bit0 rigged,1 alpha,2 mask,3 hud,7 bounds-unknown,9 attach
    uint32_t skin_id;         //   4
};
static_assert(sizeof(Instance) == 192, "Instance must be 192 B (LLCleanInstanceStore::Instance)");

// LLCleanMeshAsset::SlotInfo — 32 B.
struct SlotInfo {
    uint32_t first_index;
    uint32_t index_count;     // 0 => not resident
    int32_t  base_vertex;
    uint32_t has_weights;
    uint32_t vert_count;
    uint32_t _pad0, _pad1, _pad2;
};
static_assert(sizeof(SlotInfo) == 32, "SlotInfo must be 32 B (LLCleanMeshAsset::SlotInfo)");

// LLCleanMeshAsset::LodGroup — 16 B (uvec4).
struct LodGroup { uint32_t slot[NUM_LOD]; };
static_assert(sizeof(LodGroup) == 16, "LodGroup must be 16 B");

// LLCleanMaterialStore::Material — 96 B. In GL the *_handle fields are GLuint64
// bindless handles; in the Vulkan-native design the handle's low 32 bits carry a
// descriptor-array index. Layout/size stay identical (two u32 per handle).
struct Material {
    uint32_t base_color_handle[2];   // .x = texture index (Vulkan)
    uint32_t normal_handle[2];
    uint32_t mr_handle[2];
    uint32_t emissive_handle[2];
    float base_color_factor[4];
    float emissive_factor[4];
    float spec_color_gloss[4];
    float metallic_factor;
    float roughness_factor;
    float alpha_cutoff;
    uint32_t flags;                  // bit0 has_base ...
};
static_assert(sizeof(Material) == 96, "Material must be 96 B (LLCleanMaterialStore::Material)");

// VkDrawIndexedIndirectCommand.
struct DrawCmd {
    uint32_t indexCount;
    uint32_t instanceCount;
    uint32_t firstIndex;
    int32_t  vertexOffset;
    uint32_t firstInstance;
};

// Push constant for the cull compute. Mirrors clean_cull.comp's Push block.
struct CullPush {
    glm::vec4 planes[6];
    glm::vec4 camPos;       // .xyz this-pass cam, .w lod_factor
    glm::vec4 lodCamPos;    // .xyz main-camera cam (LOD distance)
    uint32_t  instanceCount;
    uint32_t  bucketCap;
    uint32_t  idxValid;
    uint32_t  hudPass;
};

struct DrawVertex { glm::vec3 pos; glm::vec2 uv; glm::vec3 normal; };

// ---------------------------------------------------------------------------
// Frustum plane extraction (Gribb/Hartmann), inside if dot(n,p)+d >= 0.
// ---------------------------------------------------------------------------
void extractPlanes(const glm::mat4& m, glm::vec4 planes[6]) {
    glm::vec4 r0(m[0][0], m[1][0], m[2][0], m[3][0]);
    glm::vec4 r1(m[0][1], m[1][1], m[2][1], m[3][1]);
    glm::vec4 r2(m[0][2], m[1][2], m[2][2], m[3][2]);
    glm::vec4 r3(m[0][3], m[1][3], m[2][3], m[3][3]);
    planes[0] = r3 + r0; // left
    planes[1] = r3 - r0; // right
    planes[2] = r3 + r1; // bottom
    planes[3] = r3 - r1; // top
    planes[4] = r2;      // near (ZERO_TO_ONE: row2 alone)
    planes[5] = r3 - r2; // far
    for (int i = 0; i < 6; ++i) {
        float len = glm::length(glm::vec3(planes[i]));
        if (len > 0.0f) planes[i] /= len;
    }
}

// ---------------------------------------------------------------------------
// Shared proto geometry: a set of LOD "meshes" packed into a mega-VBO / mega-IBO
// exactly as LLCleanMeshAsset would. Each LOD is a subdivided cube (more tris at
// higher LOD) so the SlotInfo first_index/index_count/base_vertex are meaningful
// and the LOD select visibly changes triangle counts. We store the 4 LODs of ONE
// canonical mesh; a resident LOD group references its 4 slots, an unresident one
// leaves some slots at SENTINEL / index_count 0.
// ---------------------------------------------------------------------------
struct MegaGeom {
    std::vector<DrawVertex> verts;
    std::vector<uint32_t>   indices;
    // Per-LOD slot payload (populated into the SlotInfo SSBO).
    SlotInfo lodSlot[NUM_LOD];
};

void appendCube(MegaGeom& g, uint32_t subdiv, SlotInfo& slotOut) {
    uint32_t vbase = (uint32_t)g.verts.size();
    uint32_t ibase = (uint32_t)g.indices.size();
    const glm::vec3 faceN[6] = {{1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1}};
    // For a normal-aligned face build a subdiv x subdiv grid.
    auto axis = [](glm::vec3 n, glm::vec3& u, glm::vec3& v) {
        if (std::fabs(n.x) > 0.5f)      { u = {0,1,0}; v = {0,0,1}; }
        else if (std::fabs(n.y) > 0.5f) { u = {1,0,0}; v = {0,0,1}; }
        else                            { u = {1,0,0}; v = {0,1,0}; }
    };
    const float s = 0.5f;
    for (int f = 0; f < 6; ++f) {
        glm::vec3 n = faceN[f];
        glm::vec3 u, v; axis(n, u, v);
        glm::vec3 center = n * s;
        uint32_t faceV = (uint32_t)g.verts.size();
        for (uint32_t j = 0; j <= subdiv; ++j)
            for (uint32_t i = 0; i <= subdiv; ++i) {
                float fu = (float)i/subdiv * 2.0f - 1.0f;
                float fv = (float)j/subdiv * 2.0f - 1.0f;
                glm::vec3 p = center + u*(fu*s) + v*(fv*s);
                DrawVertex dv;
                dv.pos = p;
                dv.uv = glm::vec2((float)i/subdiv, (float)j/subdiv);
                dv.normal = n;
                g.verts.push_back(dv);
            }
        uint32_t row = subdiv + 1;
        for (uint32_t j = 0; j < subdiv; ++j)
            for (uint32_t i = 0; i < subdiv; ++i) {
                uint32_t a = faceV + j*row + i;
                uint32_t b = a + 1;
                uint32_t c = a + row;
                uint32_t d = c + 1;
                // face-local indices (0-based from vbase), like the mega-IBO
                g.indices.push_back(a - vbase); g.indices.push_back(b - vbase); g.indices.push_back(c - vbase);
                g.indices.push_back(b - vbase); g.indices.push_back(d - vbase); g.indices.push_back(c - vbase);
            }
    }
    uint32_t vcount = (uint32_t)g.verts.size() - vbase;
    uint32_t icount = (uint32_t)g.indices.size() - ibase;
    slotOut.first_index = ibase;
    slotOut.index_count = icount;
    slotOut.base_vertex = (int32_t)vbase;
    slotOut.has_weights = 0;
    slotOut.vert_count  = vcount;
    slotOut._pad0 = slotOut._pad1 = slotOut._pad2 = 0;
}

// ---------------------------------------------------------------------------
// CPU reference cull — a line-for-line duplicate of clean_cull.comp so the GPU
// bucket counts can be checked exactly.
// ---------------------------------------------------------------------------
uint32_t detailFromTanCPU(float t) {
    t = std::floor(t * 100.0f + 0.5f) * 0.01f;
    if (t <= 0.03f) return 0;
    if (t <= 0.06f) return 1;
    if (t <= 0.24f) return 2;
    return 3;
}

glm::mat4 instMat(const Instance& in) {
    glm::mat4 m;
    std::memcpy(&m[0][0], in.model_matrix, sizeof(float)*16);
    return m;
}

void cpuCull(const std::vector<Instance>& insts, const std::vector<SlotInfo>& slots,
             const std::vector<LodGroup>& groups, const CullPush& pc,
             uint32_t idxValid, uint32_t hudPass, uint32_t out_bucket[6]) {
    for (int b = 0; b < 6; ++b) out_bucket[b] = 0;
    for (uint32_t id = 0; id < insts.size(); ++id) {
        const Instance& I = insts[id];
        if (I.lod_group_id == LOD_SENTINEL) continue;

        glm::mat4 mm = instMat(I);
        bool bad = false;
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 3; ++r) { float x = mm[c][r]; if (std::isnan(x)||std::isinf(x)) bad = true; }
        float msc = std::max({glm::length(glm::vec3(mm[0])), glm::length(glm::vec3(mm[1])), glm::length(glm::vec3(mm[2]))});
        if (msc > 1e6f) bad = true;
        if (std::fabs(mm[3][0])>1e7f||std::fabs(mm[3][1])>1e7f||std::fabs(mm[3][2])>1e7f) bad = true;
        if (bad) continue;

        uint32_t flags = I.flags;
        bool is_hud = (flags & 8u) != 0u;
        if (hudPass == 0u) { if (is_hud) continue; }
        else               { if (!is_hud) continue; }

        bool rigged = (flags & 1u) != 0u && I.skin_id != 0xFFFFFFFFu;
        bool bounds_unknown = (flags & 128u) != 0u;
        bool attach = (flags & 512u) != 0u;

        if (!rigged && !bounds_unknown && !attach) {
            glm::vec3 lo(I.aabb_min[0], I.aabb_min[1], I.aabb_min[2]);
            glm::vec3 hi(I.aabb_max[0], I.aabb_max[1], I.aabb_max[2]);
            glm::vec3 wmin(1e30f), wmax(-1e30f);
            for (int i = 0; i < 8; ++i) {
                glm::vec3 c((i&1)?hi.x:lo.x, (i&2)?hi.y:lo.y, (i&4)?hi.z:lo.z);
                glm::vec3 w = glm::vec3(mm * glm::vec4(c,1.0f));
                wmin = glm::min(wmin, w); wmax = glm::max(wmax, w);
            }
            wmin -= glm::vec3(1.0f); wmax += glm::vec3(1.0f);
            glm::vec3 ext = wmax - wmin;
            bool degenerate = (ext.x+ext.y+ext.z) < 0.001f;
            glm::vec3 org(mm[3][0], mm[3][1], mm[3][2]);
            bool origin_visible = true;
            for (int i = 0; i < 6; ++i) {
                glm::vec4 p = pc.planes[i];
                if (glm::dot(glm::vec3(p), org) + p.w < -2.0f) { origin_visible = false; break; }
            }
            bool frust = true;
            for (int i = 0; i < 6; ++i) {
                glm::vec4 p = pc.planes[i];
                glm::vec3 vtx(p.x>=0?wmax.x:wmin.x, p.y>=0?wmax.y:wmin.y, p.z>=0?wmax.z:wmin.z);
                if (glm::dot(glm::vec3(p), vtx) + p.w < 0.0f) { frust = false; break; }
            }
            if (!degenerate && !origin_visible && !frust) continue;
        }

        uint32_t slot_id;
        if (rigged || is_hud || attach) {
            slot_id = LOD_SENTINEL;
            for (int L = 3; L >= 0; --L) { uint32_t s = groups[I.lod_group_id].slot[L]; if (s != LOD_SENTINEL) { slot_id = s; break; } }
        } else {
            float radius = I.aabb_min[3];
            glm::vec3 origin(mm[3][0], mm[3][1], mm[3][2]);
            float dist = std::max(glm::length(origin - glm::vec3(pc.lodCamPos)), 0.001f);
            uint32_t level = detailFromTanCPU((pc.camPos.w * radius) / dist);
            slot_id = groups[I.lod_group_id].slot[level];
        }
        if (slot_id == LOD_SENTINEL) continue;

        const SlotInfo& s = slots[slot_id];
        if (s.index_count != 0u && (s.first_index + s.index_count) > idxValid) continue;
        if (s.index_count == 0u) continue;

        uint32_t sub = 0;
        if ((flags & 2u) != 0u) sub = 2; else if ((flags & 4u) != 0u) sub = 1;
        uint32_t bucket = rigged ? (3u + sub) : sub;
        out_bucket[bucket]++;
    }
}

// ===========================================================================
// The renderer.
// ===========================================================================
struct Clean {
    VkDeviceContext* ctx = nullptr;

    // Mega geometry.
    GpuBuffer vbo{}, ibo{};
    uint32_t  idxValid = 0;

    // Stores.
    GpuBuffer instanceSSBO{};      // persistent, uploaded once (real Instance layout)
    GpuBuffer slotSSBO{};          // SlotInfo table
    GpuBuffer lodGroupSSBO{};      // LodGroup table
    GpuBuffer materialSSBO{};      // Material store

    // Cull outputs: 6 bucket command buffers + 6-entry counter buffer.
    GpuBuffer cmd[NUM_BUCKETS]{};
    GpuBuffer counters{};
    uint32_t  bucketCap = 0;

    std::vector<GpuImage> textures;
    VkSampler sampler = VK_NULL_HANDLE;

    // descriptor set 0: SSBOs (instance/slot/cmds/counters/lodgroups/materials)
    VkDescriptorSetLayout dslCull = VK_NULL_HANDLE;
    VkDescriptorSet       dsetCull = VK_NULL_HANDLE;
    // descriptor set for draw: instance + material SSBO
    VkDescriptorSetLayout dslDraw0 = VK_NULL_HANDLE;
    VkDescriptorSet       dsetDraw0 = VK_NULL_HANDLE;
    VkDescriptorSetLayout dslDraw1 = VK_NULL_HANDLE; // bindless
    VkDescriptorSet       dsetDraw1 = VK_NULL_HANDLE;
    VkDescriptorPool      pool = VK_NULL_HANDLE;

    VkPipelineLayout cullLayout = VK_NULL_HANDLE;
    VkPipeline       cullPipe = VK_NULL_HANDLE;
    VkPipelineLayout drawLayout = VK_NULL_HANDLE;
    VkPipeline       drawPipe = VK_NULL_HANDLE;

    glm::mat4 viewProj{1.0f};
    CullPush  push{};

    std::vector<Instance> cpuInstances;
    std::vector<SlotInfo> cpuSlots;
    std::vector<LodGroup> cpuGroups;

    void destroy();
};

GpuImage makeTexture(VkDeviceContext& ctx, glm::vec3 tint) {
    const uint32_t TW = 32, TH = 32;
    std::vector<uint8_t> px(TW*TH*4);
    for (uint32_t y = 0; y < TH; ++y)
        for (uint32_t x = 0; x < TW; ++x) {
            bool check = ((x/4)+(y/4)) & 1;
            float k = check ? 1.0f : 0.55f;
            uint8_t* p = &px[(y*TW+x)*4];
            p[0]=(uint8_t)(255*tint.r*k); p[1]=(uint8_t)(255*tint.g*k);
            p[2]=(uint8_t)(255*tint.b*k); p[3]=255;
        }
    GpuImage img = ctx.createImage2D(TW,TH,VK_FORMAT_R8G8B8A8_UNORM,
        VK_IMAGE_USAGE_SAMPLED_BIT|VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    GpuBuffer stg = ctx.createBuffer(px.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    std::memcpy(stg.mapped, px.data(), px.size());
    ctx.submitImmediate([&](VkCommandBuffer cmd){
        VkImageMemoryBarrier2 a{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        a.srcStageMask=VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT; a.dstStageMask=VK_PIPELINE_STAGE_2_COPY_BIT;
        a.dstAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT; a.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
        a.newLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; a.image=img.image;
        a.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkDependencyInfo d1{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d1.imageMemoryBarrierCount=1; d1.pImageMemoryBarriers=&a;
        vkCmdPipelineBarrier2(cmd,&d1);
        VkBufferImageCopy r{}; r.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent={TW,TH,1};
        vkCmdCopyBufferToImage(cmd, stg.buffer, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r);
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask=VK_PIPELINE_STAGE_2_COPY_BIT; b.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
        b.dstStageMask=VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT; b.dstAccessMask=VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
        b.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL; b.newLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.image=img.image; b.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkDependencyInfo d2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; d2.imageMemoryBarrierCount=1; d2.pImageMemoryBarriers=&b;
        vkCmdPipelineBarrier2(cmd,&d2);
    });
    ctx.destroyBuffer(stg);
    return img;
}

void setIdentity(float m[16]) {
    std::memset(m, 0, sizeof(float)*16);
    m[0]=m[5]=m[10]=m[15]=1.0f;
}
void setTRS(float m[16], glm::vec3 t, float scale, float rotDeg, glm::vec3 axis) {
    glm::mat4 M = glm::translate(glm::mat4(1.0f), t);
    M = glm::rotate(M, glm::radians(rotDeg), axis);
    M = glm::scale(M, glm::vec3(scale));
    std::memcpy(m, &M[0][0], sizeof(float)*16);
}

// ---------------------------------------------------------------------------
// Build the scene: several LOD groups, a few hundred instances spread over a
// world grid (in view), a block deliberately BEHIND the camera (frustum-culled),
// an alpha-blend row, an alpha-mask row, a rigged block (routes to rigged
// buckets, frustum-skipped), a HUD instance (world pass must skip it), and a
// group whose finest LODs are UNRESIDENT so the cull's residency + LOD select
// picks a coarser slot.
// ---------------------------------------------------------------------------
void buildScene(Clean& c, MegaGeom& geom, uint32_t& outMaterials) {
    // ---- mega geometry: 4 LODs of the canonical mesh ----
    const uint32_t subdivs[NUM_LOD] = {1, 2, 4, 8}; // L0 coarse .. L3 fine
    for (uint32_t L = 0; L < NUM_LOD; ++L)
        appendCube(geom, subdivs[L], geom.lodSlot[L]);

    // SlotInfo table: slots 0..3 = mesh A LODs, slots 4..7 = mesh B LODs (reuse
    // mesh A geometry but as a separate group so we exercise multiple groups).
    c.cpuSlots.clear();
    for (uint32_t L = 0; L < NUM_LOD; ++L) c.cpuSlots.push_back(geom.lodSlot[L]);   // slots 0..3
    for (uint32_t L = 0; L < NUM_LOD; ++L) c.cpuSlots.push_back(geom.lodSlot[L]);   // slots 4..7 (group B)
    // Slot 8: an OOB slot (index range past idxValid) — the cull must DROP it.
    {
        SlotInfo oob = geom.lodSlot[3];
        oob.first_index = 0; oob.index_count = 0x7fffffffu; // absurd -> OOB
        c.cpuSlots.push_back(oob); // slot 8
    }

    // ---- LOD groups ----
    // group 0: mesh A, all 4 LODs resident (slots 0..3).
    // group 1: mesh B, only L0/L1 resident (slots 4,5); L2/L3 SENTINEL.
    // group 2: fully UNRESIDENT (all SENTINEL) — every instance invisible.
    // group 3: rigged/HUD target — all resident (slots 0..3), best-LOD path.
    // group 4: references the OOB slot at L3 — cull's OOB guard must drop it,
    //          falls back to L0..L2 by distance.
    c.cpuGroups.clear();
    LodGroup g0{{0,1,2,3}};                                  c.cpuGroups.push_back(g0);
    LodGroup g1{{4,5,LOD_SENTINEL,LOD_SENTINEL}};            c.cpuGroups.push_back(g1);
    LodGroup g2{{LOD_SENTINEL,LOD_SENTINEL,LOD_SENTINEL,LOD_SENTINEL}}; c.cpuGroups.push_back(g2);
    LodGroup g3{{0,1,2,3}};                                  c.cpuGroups.push_back(g3);
    LodGroup g4{{0,1,2,8}};                                  c.cpuGroups.push_back(g4);

    // ---- materials ----
    // 4 distinct textured materials (bindless indices 0..3) + factors.
    outMaterials = 4;
    std::vector<Material> mats(4);
    const glm::vec3 mtint[4] = {{1,0.5f,0.4f},{0.4f,1,0.5f},{0.5f,0.6f,1},{1,0.85f,0.35f}};
    for (int i = 0; i < 4; ++i) {
        Material m{};
        m.base_color_handle[0] = (uint32_t)i;  // bindless texture index
        m.base_color_factor[0]=1; m.base_color_factor[1]=1; m.base_color_factor[2]=1; m.base_color_factor[3]=1;
        m.metallic_factor=0; m.roughness_factor=1; m.alpha_cutoff=-1;
        m.flags = 1u; // has_base
        (void)mtint;
        mats[i] = m;
    }
    c.materialSSBO = c.ctx->createDeviceLocalWithData(mats.data(), mats.size()*sizeof(Material),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    // ---- instances (real Instance layout) ----
    c.cpuInstances.clear();
    auto pushInstance = [&](glm::vec3 pos, float scale, uint32_t group, uint32_t mat,
                            uint32_t flags, float radius, uint32_t skin_id) {
        Instance in{};
        setTRS(in.model_matrix, pos, scale, 20.0f, glm::vec3(0.3f,1.0f,0.2f));
        // local AABB of the unit cube (+/-0.5), scaled by `scale`.
        float e = 0.5f * scale;
        in.aabb_min[0]=-e; in.aabb_min[1]=-e; in.aabb_min[2]=-e; in.aabb_min[3]=radius; // .w = LOD radius
        in.aabb_max[0]= e; in.aabb_max[1]= e; in.aabb_max[2]= e; in.aabb_max[3]=0.0f;
        in.tint[0]=1; in.tint[1]=1; in.tint[2]=1; in.tint[3]=1;
        setIdentity(in.uv_matrix);
        in.lod_group_id=group; in.material_id=mat; in.flags=flags; in.skin_id=skin_id;
        c.cpuInstances.push_back(in);
    };

    // Main visible field: 12x12 grid of group-0 opaque instances in front of cam.
    const float sp = 3.0f; const int N = 12;
    float half = (N-1)*0.5f*sp;
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            glm::vec3 p(i*sp - half, j*sp - half, -6.0f);
            uint32_t mat = (uint32_t)((i + j) & 3);
            // vary LOD radius so distance-based LOD picks different levels
            float radius = 2.0f + 0.15f*((i*7+j*13)%20);
            pushInstance(p, 1.0f, 0, mat, /*flags*/0u, radius, 0xFFFFFFFFu);
        }

    // Behind-camera block (must be frustum-culled): group 0, opaque.
    for (int k = 0; k < 40; ++k)
        pushInstance(glm::vec3((k%8)*sp - half, (k/8)*sp, 40.0f), 1.0f, 0, 0, 0u, 3.0f, 0xFFFFFFFFu);

    // Alpha-blend row (flag bit1 = 2) -> alpha bucket.
    for (int i = 0; i < 16; ++i)
        pushInstance(glm::vec3(i*sp - half, half + sp, -8.0f), 1.0f, 0, 2, 2u, 4.0f, 0xFFFFFFFFu);

    // Alpha-mask row (flag bit2 = 4) -> mask bucket.
    for (int i = 0; i < 16; ++i)
        pushInstance(glm::vec3(i*sp - half, -half - sp, -8.0f), 1.0f, 0, 1, 4u, 4.0f, 0xFFFFFFFFu);

    // Group 1 (only L0/L1 resident): a near cluster wants L3 (unresident) so the
    // cull emits nothing for it; a far cluster wants L0/L1 -> visible.
    for (int i = 0; i < 8; ++i) // near -> unresident LOD -> culled by residency
        pushInstance(glm::vec3(-half + i*sp, 0.0f, -3.0f), 1.0f, 1, 3, 0u, 12.0f, 0xFFFFFFFFu);
    for (int i = 0; i < 8; ++i) // far -> L0/L1 resident -> visible
        pushInstance(glm::vec3(-half + i*sp, 4.0f, -34.0f), 1.0f, 1, 3, 0u, 1.0f, 0xFFFFFFFFu);

    // Group 2 fully unresident: 10 instances, all invisible.
    for (int i = 0; i < 10; ++i)
        pushInstance(glm::vec3(i*sp - half, 6.0f, -10.0f), 1.0f, 2, 0, 0u, 3.0f, 0xFFFFFFFFu);

    // Rigged block (flag bit0 = 1, valid skin) -> r_opaque bucket, frustum-skipped.
    for (int i = 0; i < 12; ++i)
        pushInstance(glm::vec3(i*sp - half, -6.0f, -12.0f), 1.0f, 3, 3, 1u, 3.0f, /*skin*/0u);

    // HUD instance (flag bit3 = 8): world pass must SKIP it (would count in a HUD pass).
    pushInstance(glm::vec3(0,0,-2.0f), 0.5f, 3, 0, 8u, 3.0f, 0xFFFFFFFFu);

    // Group 4 (OOB L3 slot): a near cluster whose distance picks L3 (OOB, dropped)
    // and one whose distance picks a valid level.
    for (int i = 0; i < 6; ++i)
        pushInstance(glm::vec3(-half + i*sp, 8.0f, -50.0f), 1.0f, 4, 1, 0u, 1.0f, 0xFFFFFFFFu);

    c.idxValid = (uint32_t)geom.indices.size();
}

// Write descriptor set 0 (cull): instance, slot, 6 cmds, counters, lodgroups.
void writeCullSet(Clean& c) {
    VkDescriptorBufferInfo bi[10];
    bi[0] = {c.instanceSSBO.buffer, 0, VK_WHOLE_SIZE};
    bi[1] = {c.slotSSBO.buffer, 0, VK_WHOLE_SIZE};
    for (uint32_t b = 0; b < NUM_BUCKETS; ++b) bi[2+b] = {c.cmd[b].buffer, 0, VK_WHOLE_SIZE};
    bi[8] = {c.counters.buffer, 0, VK_WHOLE_SIZE};
    bi[9] = {c.lodGroupSSBO.buffer, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet w[10]{};
    for (int i = 0; i < 10; ++i) {
        w[i].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[i].dstSet=c.dsetCull;
        w[i].dstBinding=i; w[i].descriptorCount=1; w[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[i].pBufferInfo=&bi[i];
    }
    vkUpdateDescriptorSets(c.ctx->device(), 10, w, 0, nullptr);
}

void setup(Clean& c, VkDeviceContext& ctx, const std::string& dir, uint32_t w, uint32_t h) {
    c.ctx = &ctx;
    MegaGeom geom;
    uint32_t numMaterials = 0;
    buildScene(c, geom, numMaterials);

    c.vbo = ctx.createDeviceLocalWithData(geom.verts.data(), geom.verts.size()*sizeof(DrawVertex),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    c.ibo = ctx.createDeviceLocalWithData(geom.indices.data(), geom.indices.size()*sizeof(uint32_t),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    c.instanceSSBO = ctx.createDeviceLocalWithData(c.cpuInstances.data(),
        c.cpuInstances.size()*sizeof(Instance), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    c.slotSSBO = ctx.createDeviceLocalWithData(c.cpuSlots.data(),
        c.cpuSlots.size()*sizeof(SlotInfo), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    c.lodGroupSSBO = ctx.createDeviceLocalWithData(c.cpuGroups.data(),
        c.cpuGroups.size()*sizeof(LodGroup), VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);

    // Bucket command buffers + counters. Cap generously.
    c.bucketCap = (uint32_t)c.cpuInstances.size();
    for (uint32_t b = 0; b < NUM_BUCKETS; ++b)
        c.cmd[b] = ctx.createBuffer(c.bucketCap*sizeof(DrawCmd),
            VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    c.counters = ctx.createBuffer(NUM_BUCKETS*sizeof(uint32_t),
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT
        |VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    // Textures.
    const glm::vec3 tints[4] = {{1,0.5f,0.4f},{0.4f,1,0.5f},{0.5f,0.6f,1},{1,0.85f,0.35f}};
    for (uint32_t i = 0; i < numMaterials; ++i) c.textures.push_back(makeTexture(ctx, tints[i]));
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    sci.magFilter=VK_FILTER_LINEAR; sci.minFilter=VK_FILTER_LINEAR;
    VK_CHECK(vkCreateSampler(ctx.device(), &sci, nullptr, &c.sampler));

    // ---- camera ----
    glm::mat4 view = glm::lookAt(glm::vec3(0,0,22), glm::vec3(0,0,-10), glm::vec3(0,1,0));
    glm::mat4 proj = glm::perspective(glm::radians(60.0f), (float)w/h, 0.5f, 300.0f);
    proj[1][1] *= -1.0f;
    c.viewProj = proj * view;
    glm::mat4 projU = glm::perspective(glm::radians(60.0f), (float)w/h, 0.5f, 300.0f);
    extractPlanes(projU * view, c.push.planes);
    c.push.camPos    = glm::vec4(0,0,22, /*lod_factor*/ 1.125f);
    c.push.lodCamPos = glm::vec4(0,0,22, 0);
    c.push.instanceCount = (uint32_t)c.cpuInstances.size();
    c.push.bucketCap = c.bucketCap;
    c.push.idxValid  = c.idxValid;
    c.push.hudPass   = 0;

    // ---- cull descriptor layout: 10 storage buffers ----
    VkDescriptorSetLayoutBinding cb[10]{};
    for (int i = 0; i < 10; ++i) {
        cb[i].binding=i; cb[i].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        cb[i].descriptorCount=1; cb[i].stageFlags=VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo cl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    cl.bindingCount=10; cl.pBindings=cb;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &cl, nullptr, &c.dslCull));

    // ---- draw set 0: instance + material SSBO (vertex + fragment) ----
    VkDescriptorSetLayoutBinding db0[2]{};
    db0[0].binding=0; db0[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; db0[0].descriptorCount=1;
    db0[0].stageFlags=VK_SHADER_STAGE_VERTEX_BIT;
    db0[1].binding=1; db0[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; db0[1].descriptorCount=1;
    db0[1].stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo dl0{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl0.bindingCount=2; dl0.pBindings=db0;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dl0, nullptr, &c.dslDraw0));

    // ---- draw set 1: bindless texture array ----
    VkDescriptorSetLayoutBinding db1{};
    db1.binding=0; db1.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    db1.descriptorCount=numMaterials; db1.stageFlags=VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorBindingFlags bf = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO};
    bfi.bindingCount=1; bfi.pBindingFlags=&bf;
    VkDescriptorSetLayoutCreateInfo dl1{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dl1.pNext=&bfi; dl1.bindingCount=1; dl1.pBindings=&db1;
    VK_CHECK(vkCreateDescriptorSetLayout(ctx.device(), &dl1, nullptr, &c.dslDraw1));

    // ---- pool + sets ----
    VkDescriptorPoolSize psz[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 12},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, numMaterials}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets=3; pci.poolSizeCount=2; pci.pPoolSizes=psz;
    VK_CHECK(vkCreateDescriptorPool(ctx.device(), &pci, nullptr, &c.pool));

    auto alloc = [&](VkDescriptorSetLayout l, VkDescriptorSet& s){
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool=c.pool; ai.descriptorSetCount=1; ai.pSetLayouts=&l;
        VK_CHECK(vkAllocateDescriptorSets(ctx.device(), &ai, &s));
    };
    alloc(c.dslCull, c.dsetCull);
    alloc(c.dslDraw0, c.dsetDraw0);
    alloc(c.dslDraw1, c.dsetDraw1);

    writeCullSet(c);

    // draw set 0
    {
        VkDescriptorBufferInfo bi0{c.instanceSSBO.buffer,0,VK_WHOLE_SIZE};
        VkDescriptorBufferInfo bi1{c.materialSSBO.buffer,0,VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[2]{};
        w[0].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[0].dstSet=c.dsetDraw0; w[0].dstBinding=0;
        w[0].descriptorCount=1; w[0].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[0].pBufferInfo=&bi0;
        w[1].sType=VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET; w[1].dstSet=c.dsetDraw0; w[1].dstBinding=1;
        w[1].descriptorCount=1; w[1].descriptorType=VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; w[1].pBufferInfo=&bi1;
        vkUpdateDescriptorSets(ctx.device(), 2, w, 0, nullptr);
    }
    // draw set 1 (bindless)
    {
        std::vector<VkDescriptorImageInfo> ii(numMaterials);
        for (uint32_t i = 0; i < numMaterials; ++i) {
            ii[i].sampler=c.sampler; ii[i].imageView=c.textures[i].view;
            ii[i].imageLayout=VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        }
        VkWriteDescriptorSet wt{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        wt.dstSet=c.dsetDraw1; wt.dstBinding=0; wt.descriptorCount=numMaterials;
        wt.descriptorType=VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER; wt.pImageInfo=ii.data();
        vkUpdateDescriptorSets(ctx.device(), 1, &wt, 0, nullptr);
    }

    // ---- cull pipeline ----
    VkPushConstantRange cpcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CullPush)};
    VkPipelineLayoutCreateInfo cli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    cli.setLayoutCount=1; cli.pSetLayouts=&c.dslCull; cli.pushConstantRangeCount=1; cli.pPushConstantRanges=&cpcr;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &cli, nullptr, &c.cullLayout));
    VkShaderModule cs = loadShaderModule(ctx.device(), dir + "/clean_cull.comp.spv");
    c.cullPipe = createComputePipeline(ctx.device(), cs, c.cullLayout);
    vkDestroyShaderModule(ctx.device(), cs, nullptr);

    // ---- draw pipeline ----
    VkDescriptorSetLayout dsets[2] = {c.dslDraw0, c.dslDraw1};
    VkPushConstantRange dpcr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4)};
    VkPipelineLayoutCreateInfo dli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    dli.setLayoutCount=2; dli.pSetLayouts=dsets; dli.pushConstantRangeCount=1; dli.pPushConstantRanges=&dpcr;
    VK_CHECK(vkCreatePipelineLayout(ctx.device(), &dli, nullptr, &c.drawLayout));

    VkShaderModule vs = loadShaderModule(ctx.device(), dir + "/clean_draw.vert.spv");
    VkShaderModule fs = loadShaderModule(ctx.device(), dir + "/clean_draw.frag.spv");
    GraphicsPipelineDesc gd;
    gd.vert=vs; gd.frag=fs; gd.layout=c.drawLayout;
    gd.colorFormat=VK_FORMAT_R8G8B8A8_UNORM; gd.depthFormat=VK_FORMAT_D32_SFLOAT;
    gd.depthTest=true; gd.depthWrite=true; gd.cullMode=VK_CULL_MODE_NONE;
    gd.bindings={{0,sizeof(DrawVertex),VK_VERTEX_INPUT_RATE_VERTEX}};
    gd.attributes={
        {0,0,VK_FORMAT_R32G32B32_SFLOAT, offsetof(DrawVertex,pos)},
        {1,0,VK_FORMAT_R32G32_SFLOAT,    offsetof(DrawVertex,uv)},
        {2,0,VK_FORMAT_R32G32B32_SFLOAT, offsetof(DrawVertex,normal)}};
    c.drawPipe = createGraphicsPipeline(ctx.device(), gd);
    vkDestroyShaderModule(ctx.device(), vs, nullptr);
    vkDestroyShaderModule(ctx.device(), fs, nullptr);
}

void beginRenderingCD(VkCommandBuffer cmd, OffscreenTarget& t) {
    uint32_t w=t.width(), h=t.height();
    VkImageMemoryBarrier2 col{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    col.srcStageMask=VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT; col.dstStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    col.dstAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT; col.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED;
    col.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; col.image=t.color().image;
    col.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
    VkImageMemoryBarrier2 d{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    d.srcStageMask=VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    d.dstStageMask=VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT|VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    d.dstAccessMask=VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT|VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    d.oldLayout=VK_IMAGE_LAYOUT_UNDEFINED; d.newLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    d.image=t.depth().image; d.subresourceRange={VK_IMAGE_ASPECT_DEPTH_BIT,0,1,0,1};
    VkImageMemoryBarrier2 bar[2]={col,d};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.imageMemoryBarrierCount=2; dep.pImageMemoryBarriers=bar;
    vkCmdPipelineBarrier2(cmd,&dep);

    VkRenderingAttachmentInfo ca{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    ca.imageView=t.color().view; ca.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    ca.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; ca.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
    ca.clearValue.color={{kClear.r,kClear.g,kClear.b,kClear.a}};
    VkRenderingAttachmentInfo da{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    da.imageView=t.depth().view; da.imageLayout=VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    da.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR; da.storeOp=VK_ATTACHMENT_STORE_OP_DONT_CARE;
    da.clearValue.depthStencil={1.0f,0};
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea={{0,0},{w,h}}; ri.layerCount=1; ri.colorAttachmentCount=1;
    ri.pColorAttachments=&ca; ri.pDepthAttachment=&da;
    vkCmdBeginRendering(cmd,&ri);
    VkViewport vp{0,0,(float)w,(float)h,0,1}; VkRect2D sc{{0,0},{w,h}};
    vkCmdSetViewport(cmd,0,1,&vp); vkCmdSetScissor(cmd,0,1,&sc);
}

void recordFrame(VkCommandBuffer cmd, Clean& c, OffscreenTarget& t) {
    // Zero the 6 bucket counters, barrier into compute.
    vkCmdFillBuffer(cmd, c.counters.buffer, 0, NUM_BUCKETS*sizeof(uint32_t), 0);
    {
        VkBufferMemoryBarrier2 fb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
        fb.srcStageMask=VK_PIPELINE_STAGE_2_CLEAR_BIT; fb.srcAccessMask=VK_ACCESS_2_TRANSFER_WRITE_BIT;
        fb.dstStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        fb.dstAccessMask=VK_ACCESS_2_SHADER_STORAGE_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        fb.buffer=c.counters.buffer; fb.size=VK_WHOLE_SIZE;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.bufferMemoryBarrierCount=1; dep.pBufferMemoryBarriers=&fb;
        vkCmdPipelineBarrier2(cmd,&dep);
    }

    // Compute cull.
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.cullPipe);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, c.cullLayout, 0, 1, &c.dsetCull, 0, nullptr);
    vkCmdPushConstants(cmd, c.cullLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CullPush), &c.push);
    uint32_t groups = (c.push.instanceCount + 63)/64;
    vkCmdDispatch(cmd, groups, 1, 1);

    // Compute writes (cmds + counters) -> indirect + vertex reads.
    {
        std::vector<VkBufferMemoryBarrier2> bs;
        auto mk = [&](VkBuffer buf){
            VkBufferMemoryBarrier2 b{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
            b.srcStageMask=VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT; b.srcAccessMask=VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            b.dstStageMask=VK_PIPELINE_STAGE_2_DRAW_INDIRECT_BIT|VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
            b.dstAccessMask=VK_ACCESS_2_INDIRECT_COMMAND_READ_BIT|VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
            b.buffer=buf; b.size=VK_WHOLE_SIZE; bs.push_back(b);
        };
        for (uint32_t b = 0; b < NUM_BUCKETS; ++b) mk(c.cmd[b].buffer);
        mk(c.counters.buffer);
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.bufferMemoryBarrierCount=(uint32_t)bs.size(); dep.pBufferMemoryBarriers=bs.data();
        vkCmdPipelineBarrier2(cmd,&dep);
    }

    // Draw all 6 buckets via indirect-count.
    beginRenderingCD(cmd, t);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.drawPipe);
    VkDescriptorSet dsets[2]={c.dsetDraw0, c.dsetDraw1};
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, c.drawLayout, 0, 2, dsets, 0, nullptr);
    vkCmdPushConstants(cmd, c.drawLayout, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(glm::mat4), &c.viewProj);
    VkDeviceSize off=0;
    vkCmdBindVertexBuffers(cmd,0,1,&c.vbo.buffer,&off);
    vkCmdBindIndexBuffer(cmd, c.ibo.buffer, 0, VK_INDEX_TYPE_UINT32);
    for (uint32_t b = 0; b < NUM_BUCKETS; ++b) {
        vkCmdDrawIndexedIndirectCount(cmd, c.cmd[b].buffer, 0,
            c.counters.buffer, b*sizeof(uint32_t), c.bucketCap, sizeof(DrawCmd));
    }
    vkCmdEndRendering(cmd);
}

void readCounters(Clean& c, uint32_t out[NUM_BUCKETS]) {
    GpuBuffer host = c.ctx->createBuffer(NUM_BUCKETS*sizeof(uint32_t),
        VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    c.ctx->submitImmediate([&](VkCommandBuffer cmd){
        VkBufferCopy r{0,0,NUM_BUCKETS*sizeof(uint32_t)};
        vkCmdCopyBuffer(cmd, c.counters.buffer, host.buffer, 1, &r);
    });
    std::memcpy(out, host.mapped, NUM_BUCKETS*sizeof(uint32_t));
    c.ctx->destroyBuffer(host);
}

void Clean::destroy() {
    VkDevice dev = ctx->device();
    if (drawPipe) vkDestroyPipeline(dev, drawPipe, nullptr);
    if (drawLayout) vkDestroyPipelineLayout(dev, drawLayout, nullptr);
    if (cullPipe) vkDestroyPipeline(dev, cullPipe, nullptr);
    if (cullLayout) vkDestroyPipelineLayout(dev, cullLayout, nullptr);
    if (pool) vkDestroyDescriptorPool(dev, pool, nullptr);
    if (dslCull) vkDestroyDescriptorSetLayout(dev, dslCull, nullptr);
    if (dslDraw0) vkDestroyDescriptorSetLayout(dev, dslDraw0, nullptr);
    if (dslDraw1) vkDestroyDescriptorSetLayout(dev, dslDraw1, nullptr);
    if (sampler) vkDestroySampler(dev, sampler, nullptr);
    for (auto& tx : textures) ctx->destroyImage(tx);
    ctx->destroyBuffer(vbo); ctx->destroyBuffer(ibo);
    ctx->destroyBuffer(instanceSSBO); ctx->destroyBuffer(slotSSBO);
    ctx->destroyBuffer(lodGroupSSBO); ctx->destroyBuffer(materialSSBO);
    for (uint32_t b=0;b<NUM_BUCKETS;++b) ctx->destroyBuffer(cmd[b]);
    ctx->destroyBuffer(counters);
}

// Count non-clear pixels in the PNG buffer path proxy: re-read via a host copy.
bool imageHasCoverage(VkDeviceContext& ctx, OffscreenTarget& t) {
    const uint32_t w=t.width(), h=t.height();
    const VkDeviceSize bytes=(VkDeviceSize)w*h*4;
    GpuBuffer host = ctx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT|VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    ctx.submitImmediate([&](VkCommandBuffer cmd){
        VkImageMemoryBarrier2 toSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toSrc.srcStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toSrc.srcAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toSrc.dstStageMask=VK_PIPELINE_STAGE_2_COPY_BIT; toSrc.dstAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;
        toSrc.oldLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; toSrc.newLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        toSrc.image=t.color().image; toSrc.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep.imageMemoryBarrierCount=1; dep.pImageMemoryBarriers=&toSrc;
        vkCmdPipelineBarrier2(cmd,&dep);
        VkBufferImageCopy r{}; r.imageSubresource={VK_IMAGE_ASPECT_COLOR_BIT,0,0,1}; r.imageExtent={w,h,1};
        vkCmdCopyImageToBuffer(cmd, t.color().image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, host.buffer, 1, &r);
        // restore layout so readbackToPng's own transition source is valid
        VkImageMemoryBarrier2 back{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        back.srcStageMask=VK_PIPELINE_STAGE_2_COPY_BIT; back.srcAccessMask=VK_ACCESS_2_TRANSFER_READ_BIT;
        back.dstStageMask=VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        back.dstAccessMask=VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        back.oldLayout=VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; back.newLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        back.image=t.color().image; back.subresourceRange={VK_IMAGE_ASPECT_COLOR_BIT,0,1,0,1};
        VkDependencyInfo dep2{VK_STRUCTURE_TYPE_DEPENDENCY_INFO}; dep2.imageMemoryBarrierCount=1; dep2.pImageMemoryBarriers=&back;
        vkCmdPipelineBarrier2(cmd,&dep2);
    });
    const uint8_t* px=(const uint8_t*)host.mapped;
    uint8_t cr=(uint8_t)(255*kClear.r), cg=(uint8_t)(255*kClear.g), cb=(uint8_t)(255*kClear.b);
    uint32_t nonClear=0;
    for (uint32_t i=0;i<w*h;++i){
        uint8_t r=px[i*4],g=px[i*4+1],b=px[i*4+2];
        if (std::abs((int)r-cr)>10||std::abs((int)g-cg)>10||std::abs((int)b-cb)>10) ++nonClear;
    }
    ctx.destroyBuffer(host);
    return nonClear > (w*h)/100;   // >1% of the frame has geometry
}

} // namespace

bool sceneClean(VkDeviceContext& ctx, OffscreenTarget& t,
                const std::string& shaderDir, const std::string& out_png,
                CleanResult& out) {
    uint32_t errBefore = ctx.validationErrorCount();
    uint32_t warnBefore = ctx.validationWarnCount();

    Clean c;
    setup(c, ctx, shaderDir, t.width(), t.height());
    out.instanceCount = (uint32_t)c.cpuInstances.size();

    // CPU reference cull (world pass).
    cpuCull(c.cpuInstances, c.cpuSlots, c.cpuGroups, c.push, c.idxValid, /*hud*/0, out.cpu_bucket);

    ctx.submitImmediate([&](VkCommandBuffer cmd){ recordFrame(cmd, c, t); });

    readCounters(c, out.gpu_bucket);

    out.cullMatch = true;
    for (int b = 0; b < 6; ++b) if (out.gpu_bucket[b] != out.cpu_bucket[b]) out.cullMatch = false;

    out.nonEmpty = imageHasCoverage(ctx, t);
    out.rendered = t.readbackToPng(ctx, out_png);

    out.validationErrors = ctx.validationErrorCount() - errBefore;
    out.validationWarnings = ctx.validationWarnCount() - warnBefore;

    c.destroy();
    return out.cullMatch && out.rendered && out.nonEmpty;
}
