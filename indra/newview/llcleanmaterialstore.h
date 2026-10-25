/**
 * @file llcleanmaterialstore.h
 * @brief GPU-driven material store. SSBO of material entries
 *        (texture slots + PBR factors). Per-instance lookup in the
 *        shaders via material_id from the instance SSBO.
 *
 *        Each Material entry names its textures (base color, normal,
 *        MR, emissive) by UUID; the Vulkan renderer resolves them to
 *        its bindless texture indices in its copy of the entry.
 */

#pragma once

#include "stdtypes.h"
#include "lluuid.h"

#include <map>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

class LLCleanMaterialStore
{
public:
    static LLCleanMaterialStore& instance();

    bool init(U32 initial_capacity = 16384);
    void shutdown();

    // GPU layout. std430. mat4 alignment forces 16-byte boundaries.
    struct Material
    {
        // Per map: [0] = the renderer's bindless texture index, [1] = its format code.
        // The store writes 0 (no texture); the Vulkan renderer fills them when it
        // resolves the material's texture UUIDs (materialTextureIds). Shaders name
        // these fields *_handle (uvec2).
        U32 base_color_tex[2];   //  8 B
        U32 normal_tex[2];       //  8 B
        U32 mr_tex[2];           //  8 B (metallic-roughness)
        U32 emissive_tex[2];     //  8 B
        F32 base_color_factor[4]; // 16 B (rgba)
        F32 emissive_factor[4];   // 16 B (.a = env intensity for legacy mats)
        F32 spec_color_gloss[4];  // 16 B  legacy BP: rgb = specular color,
                                  //       a = glossiness (spec exponent / 255)
        F32 metallic_factor;     //  4 B
        F32 roughness_factor;    //  4 B
        F32 alpha_cutoff;        //  4 B  (<0 = no mask)
        U32 flags;               //  4 B  bit0 = has_base, bit1 = has_normal,
                                 //       bit2 = has_mr, bit3 = has_emissive
                                 //       (bits 0-3 set by the renderer at resolve),
                                 //       bit4 = legacy Blinn-Phong material
                                 //       bit5 = all slots are color (terrain)
                                 //       bit6 = glTF (PBR) material
                                 //       bit7 = legacy emissive-mask alpha (#515)
                                 //       bits[8,16) = glow byte (0-255)
                                 //       bit16 = HUD material (VK: base colour
                                 //       sampled through the UNORM view, #532)
                                 //       bits[17,22) = legacy bump preset (#19,
                                 //       TE bumpmap 1..17; 0 = none)
                                 //       bit22 = alpha blending (plain base-colour mips)
                                 //       (mr_tex holds the SPECULAR map;
                                 //       FS writes LL's legacy gbuffer encoding)
    };                           // 96 B total, 16-aligned

    // Resolve (or allocate) a material entry that matches the given
    // texture set + factors. Identical material requests collapse
    // to the same material_id (hashed by inputs). Returns the SSBO
    // index to write into LLCleanInstanceStore::Instance::material_id.
    //
    // Textures are named by UUID (the renderer resolves them to its own texture
    // indices); pass null UUID for "no texture" (default white fallback).
    struct MaterialDesc
    {
        LLUUID base_color_id;
        LLUUID normal_id;
        LLUUID mr_id;
        LLUUID emissive_id;
        F32    base_color_factor[4] = {1.f, 1.f, 1.f, 1.f};
        F32    emissive_factor[4]   = {0.f, 0.f, 0.f, 0.f};
        F32    metallic_factor  = 0.f;
        F32    roughness_factor = 1.f;
        F32    alpha_cutoff     = -1.f;
        // Legacy Blinn-Phong (te->getMaterialParams()) fields. When
        // `legacy` is set, mr_id is the SPECULAR map and the shader
        // takes LL's materialF gbuffer path instead of the PBR one.
        F32    spec_color_gloss[4]  = {1.f, 1.f, 1.f, 0.f};
        F32    env_intensity    = 0.f;
        bool   legacy           = false;
        // All four texture slots carry COLOR data (terrain: detail_0..3 packed into
        // base/normal/mr/emissive). Material.flags bit5 (32). The VK upload uses it
        // to keep sRGB for every slot instead of UNORM for the normal/MR data maps.
        bool   color_all_slots  = false;
        // glTF (PBR) material: Material.flags bit6 (64). Only these take the PBR
        // BRDF in the VK shader; plain (no-material) faces shade like LL's legacy
        // diffuse, not a rough dielectric with Fresnel sheen.
        bool   gltf             = false;
        // Legacy material DIFFUSE_ALPHA_MODE_EMISSIVE: diffuse alpha is the emissive/glow
        // mask, not transparency. Material.flags bit7 (128). #515
        bool   emissive_mask    = false;
        // HUD material (#532): the face is on a HUD attachment. Material.flags bit16 (65536).
        // A HUD face draws in display space after the tonemap, like LL's FOR_HUD
        // fullbright: the VK renderer samples its base colour through the texture's
        // UNORM view (bilinear / trilinear filtering in gamma space, as LL's GL textures
        // do -- an sRGB view filters in linear light, which bolds and fogs light-on-dark
        // text) and keeps the image uncompressed. Part of the material identity: a world
        // face with the same texture must keep its sRGB (linear) view.
        bool   hud              = false;
        bool   alpha_blend      = false;
        // LEGACY BUMP PRESET (#19): the TE bumpmap code (1 = brightness, 2 = darkness,
        // 3..17 = the std_bump.ini heightfields: woodgrain .. weave) on a legacy face with
        // no normal map of its own. Material.flags bits [17,22). For 3..17 normal_id names
        // the heightfield (height in its alpha); the shader derives the tangent normal
        // from it, and for 1 / 2 from the base colour's luminance (LL normgenF), per
        // fragment. 0 = none.
        U8     bump             = 0;
        // SL per-face glow (te->getGlow(), 0..1). Quantized to 8 bits and
        // packed into Material.flags bits [8,16) -- keeps the GPU struct 96 B.
        // Drives bloom: FS writes it to the emissive MRT alpha, soften copies
        // it to scene alpha, LL's glowExtract/blur does the rest.
        F32    glow             = 0.f;
        // PER-MAP UV TRANSFORMS (slot 0 normal, 1 metallic-roughness / legacy specular,
        // 2 emissive). LL samples these maps with their OWN transform (glTF
        // KHR_texture_transform per slot; legacy LLMaterial normal/spec repeats, offset,
        // rotation), not the base-colour one. Affine u' = a u + b v + c,
        // v' = d u + e v + f as {a,b,c,d,e,f}. uv_own[k] false = sample at the
        // base-colour UV (unchanged behaviour).
        bool   uv_own[3]        = {false, false, false};
        F32    uv_xform[3][6]   = {{1,0,0,0,1,0},{1,0,0,0,1,0},{1,0,0,0,1,0}};
    };

    // VK-only companion to Material (parallel array, indexed by material_id). Two vec4
    // rows per slot: row0 = (a, b, c, own), row1 = (d, e, f, 0). Kept OUT of Material
    // so the GL clean pipeline's 96 B SSBO layout is unchanged.
    struct MaterialUV
    {
        F32 rows[3][2][4];
    };
    static_assert(sizeof(MaterialUV) == 96, "MaterialUV must be 96 B");

    // Resolve (or allocate) the material for `desc`. Every call (hit or create) stamps
    // the entry as recently wanted: a fresh id the caller has not yet written into an
    // instance row survives the sweep for the release grace (tick).
    U32 getOrCreate(const MaterialDesc& desc);

    // ---- LIFETIME (security audit: was append-only) ----------------------------------
    // Materials are REFCOUNTED by their holders: LLCleanInstanceStore rows (material_id)
    // and terrain records (mat[4]); pin() adds a permanent holder (id 0 = default white
    // is pinned at init). An entry with no holder that nobody re-asked for over the
    // release grace (#737: tick's grace_frames, from RenderVKAssetReleaseGraceSec -- a
    // wall-clock time, converted by LLCleanAssetLife::framesSpanning) is freed by tick():
    // its dedup key is dropped and the id waits for GPU retirement (LLCleanGpuRetire)
    // before it is reused, lowest id first. The grace counts from the last time the entry
    // was wanted: getOrCreate, or its last holder letting go. The table keeps its
    // high-water size (materialCount()) -- buffers keep their peak, live content
    // (liveMaterialCount()) stays bounded by what is in use.
    //
    // retarget: one holder changes its references (INVALID = none) -- old ids released,
    // new ids referenced. `trec` ids are terrain-record indices (instance flag 32).
    void retarget(U32 old_mat, U32 new_mat, U32 old_trec, U32 new_trec);
    void pin(U32 id);
    // Once per frame (renderer): sweep zero-ref candidates, retire pending frees.
    // grace_frames: an unreferenced entry is freed once unwanted for this many frames
    // (floored at kSweepMinGraceFrames).
    void tick(U32 grace_frames = kSweepGraceFrames);
    // Ids handed to NEW content since the last call (the VK mirror resets its per-id
    // resolve state for them). all_out = the list overflowed: reset every id.
    void takeRecycled(std::vector<U32>& out, bool& all_out);
    // Recycled ids are waiting for takeRecycled (the VK sync forces a full resolver
    // pass: a recycled row can be byte-identical to its old content under VK, where the
    // store's GL handle fields are always 0, so the raw-bytes compare would miss it).
    bool hasRecycled() const;

    struct Stats
    {
        U32 live = 0, high_water = 0, capacity = 0, free = 0, pending = 0, candidates = 0;
        U32 terrain_live = 0, terrain_slots = 0, terrain_free = 0;
        U64 freed_total = 0, reused_total = 0, terrain_freed_total = 0;
        U64 cpu_bytes = 0;
    };
    Stats stats() const;

    static constexpr U32 INVALID = 0xFFFFFFFFu;
    static constexpr U32 kSweepGraceFrames = 600;   // default grace (no wall-clock given)
    static constexpr U32 kSweepMinGraceFrames = 120; // floor: a fresh id reaches its row first
    static constexpr U32 kSweepEveryFrames = 30;

    // VK-only TERRAIN RECORD, one per region (keyed by region handle), draw set 0
    // binding 22. A terrain instance (flag 32) carries its record index in skin_id.
    // mode 0 = legacy 4-texture splat (the instance material packs the detail
    // textures), 1 = PBR: mat[] are four ordinary glTF materials, blended by the
    // same composition ramp. Each material samples all its maps with ONE transform
    // of the region-local XY (LL pbrterrainV: base-colour KHR transform, scale *
    // 1/RenderTerrainPBRScale), rows (a,b,c,0) / (d,e,f,0) like MaterialUV.
    // legacy = (offset.xy, 1/RenderTerrainScale, 0): uv = local.xy * scale + offset.
    struct TerrainRecord
    {
        U32 mat[4]       = {0, 0, 0, 0};  // PBR material ids (mode 1)
        U32 paint_idx    = 0xFFFFFFFFu;   // reserved: paint-map texture (none yet)
        U32 mode         = 0;             // 0 legacy, 1 PBR
        S32 pbr_detail   = 0;             // RenderTerrainPBRDetail (0 full .. -4 base only)
        U32 paint_type   = 0;             // TERRAIN_PAINT_TYPE_* (1 = paint map)
        F32 legacy[4]    = {0.f, 0.f, 1.f / 12.f, 0.f};
        F32 xform[4][2][4] = {};
    };
    static_assert(sizeof(TerrainRecord) == 176, "TerrainRecord must be 176 B (std430)");

    // Store/update the record for `region_key`; returns its index (stable per key while
    // the record lives). Unchanged records don't bump the generation. A record holds a
    // reference on its four materials; it is itself refcounted by the terrain instance
    // rows naming it and swept like a material (a region left behind frees its record).
    U32 setTerrainRecord(U64 region_key, const TerrainRecord& rec);
    const U8* cpuTerrainBytes() const
    {
        return mTerrain.empty() ? nullptr : reinterpret_cast<const U8*>(mTerrain.data());
    }
    U32 cpuTerrainCount() const { return (U32)mTerrain.size(); }
    // The live record index of `region_key` (INVALID when the region has none: no terrain
    // patch ingested yet, or swept). The terrain bake (#811) keys its table rows on it.
    U32 findTerrainRecord(U64 region_key) const;



    // DIAG: copy out a material entry + its recorded base-color UUID
    // for the texture-chain debug dump.
    bool debugGet(U32 material_id, Material& out, LLUUID& base_id_out) const;

    // HIGH-WATER id range [0, materialCount()) the VK mirror copies (freed ids inside it
    // are unreferenced and recycled); liveMaterialCount() = entries actually live.
    U32    materialCount() const { return mHighWater; }
    U32    liveMaterialCount() const { return mLiveCount; }

    // VK-MODE MIRROR (llcleanrenderervk.cpp). Material is byte-identical (96 B)
    // to the VK renderer's struct; mMaterials is the CPU shadow the SSBO copies.
    // Read-only span; no lock (render thread only). The *_tex fields are 0 here: the
    // VK renderer resolves each material's texture UUIDs (materialTextureIds) to its
    // own bindless indices in its copy; the factor/flag fields are directly usable.
    const U8* cpuMaterialBytes() const
    {
        return mMaterials.empty() ? nullptr
             : reinterpret_cast<const U8*>(mMaterials.data());
    }
    U32 cpuMaterialCount() const { return (U32)mMaterials.size(); }
    // Parallel per-material UV transforms (MaterialUV, 96 B each), same indexing.
    const U8* cpuMaterialUVBytes() const
    {
        return mMaterialUV.empty() ? nullptr
             : reinterpret_cast<const U8*>(mMaterialUV.data());
    }

    // Copy material `id`'s up-to-4 texture UUIDs (base/normal/mr/emissive) into
    // out[4]. Used by the eviction pass to map a GPU-visible material back to
    // the textures it uses. Returns false for an out-of-range id.
    // emissive_rgb (optional, 3 floats): the material's emissive factor as stored -- the
    // VK resolver zeroes it in its mirror while the emissive MAP is not resident and puts
    // it back from here once it is.
    // base_rgba (optional, 4 floats): the stored base colour factor -- the VK resolver
    // overrides it in its mirror for RenderVKDebugUnresolvedTint and restores it from here.
    bool   materialTextureIds(U32 id, LLUUID out[4], F32* emissive_rgb = nullptr, F32* base_rgba = nullptr) const;
    // DIAG (white-base flash): the first descriptor fields that differ between two live
    // materials, e.g. "alpha_cutoff -1 -> 0.33" -- what makes a face's material flip.
    std::string debugDescDiff(U32 a, U32 b) const;
    // #825: true when two live materials cast the same shadow -- what the shadow passes read
    // of a mask / blend face: the base-colour texture and factor (alpha = the cutout / the
    // translucency, rgb = the translucent-shadow tint), the alpha cutoff and the alpha
    // interpretation (legacy / glTF, emissive-mask; its base UVs are instance lanes). Glow,
    // emissive, normal / specular / metallic-roughness maps and factors, bump and shininess
    // are not.
    // False if either id is not live (unknown: treat as a change).
    bool shadowEquivalent(U32 a, U32 b) const;

private:
    LLCleanMaterialStore() = default;
    LLCleanMaterialStore(const LLCleanMaterialStore&) = delete;
    LLCleanMaterialStore& operator=(const LLCleanMaterialStore&) = delete;

    bool grow(U32 new_cap);
    static U64 hashDesc(const MaterialDesc& d);

    U32    mCapacity = 0;
    U32    mLiveCount = 0;    // live (allocated, not freed) entries
    U32    mHighWater = 0;    // ids ever handed out: [0, mHighWater)

    // Parallel CPU-only state: each material's texture UUIDs, which the renderer
    // resolves to texture indices (materialTextureIds). Kept separate from the
    // GPU-uploaded Material struct so its size doesn't affect SSBO layout.
    // mResolveState[i] corresponds to mMaterials[i].
    struct ResolveState
    {
        LLUUID base_color_id;
        LLUUID normal_id;
        LLUUID mr_id;
        LLUUID emissive_id;
    };

    std::vector<Material> mMaterials;
    std::vector<MaterialUV> mMaterialUV;   // parallel to mMaterials
    std::vector<ResolveState> mResolveState;
    std::vector<TerrainRecord> mTerrain;   // VK terrain records (binding 22)
    std::map<U64, U32>         mTerrainIndex;   // region handle -> record index
    // Terrain record lifetime (parallel to mTerrain): key, holder refs (instance rows),
    // last setTerrainRecord frame, state (0 free, 1 live, 2 pending retirement).
    std::vector<U64> mTerrainKey;
    std::vector<U32> mTerrainRefs;
    std::vector<U32> mTerrainTouch;
    std::vector<U8>  mTerrainState;
    std::set<U32>    mTerrainFree;
    std::vector<std::pair<U32, U64>> mTerrainPending;   // (index, retire stamp)
    U64 mTerrainFreedTotal = 0;
    // Bumped on every material write (create / handle refresh / grow / clear): the VK
    // mirror skips its whole-array copy + compare when this hasn't moved.
    U32                   mGeneration = 1;
public:
    U32 generation() const { return mGeneration; }
private:

    // Dedup: hash -> ids with that hash (a hit is confirmed by descEqual). A bucket list,
    // not linear probing, so a freed id's key can be erased without breaking the chain.
    std::unordered_map<U64, std::vector<U32>> mHashToIds;
    // Full descriptor per id: a hash hit is confirmed by a field-by-field compare, so
    // two different materials can never silently share an id (a 64-bit hash of
    // per-UUID word sums could collide -> a face drawn with another's material).
    std::vector<MaterialDesc> mDescs;   // parallel to mMaterials
    static bool descEqual(const MaterialDesc& a, const MaterialDesc& b);

    // Lifetime (parallel to mMaterials).
    std::vector<U64> mHashOf;     // dedup key of each live id
    std::vector<U32> mRefs;       // holders (instance rows, terrain records, pins)
    std::vector<U32> mTouch;      // gFrameCount of the last getOrCreate / last holder release
    std::vector<U8>  mState;      // 0 free, 1 live, 2 pending retirement
    std::vector<U8>  mInCand;     // queued in mCandidates
    std::vector<U32> mCandidates; // ids whose refs reached 0 (or never rose): sweep input
    std::vector<std::pair<U32, U64>> mPending;   // (id, retire stamp)
    std::set<U32>    mFree;       // retired ids, reused lowest first
    std::vector<U32> mRecycled;   // reused ids not yet seen by the VK mirror
    bool             mRecycledAll = false;
    U32              mLastSweep = 0;
    U64              mFreedTotal = 0, mReusedTotal = 0;

    void addRefLocked(U32 id);
    void releaseLocked(U32 id);
    void addCandidateLocked(U32 id);
    void trecAddRefLocked(U32 idx);
    void trecReleaseLocked(U32 idx);
    void unkeyLocked(U32 id);

    mutable std::mutex mMutex;
};
