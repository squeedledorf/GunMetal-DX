/**
 * @file llcleantexencvk.h
 * @brief GPU block compression of sampled textures for the Vulkan clean renderer
 *        (RenderVKTextureCompression, #263).
 *
 * The texture streamer (LLCleanRendererVK::uploadRealMaterialTextures) decides per image
 * whether it is stored block-compressed; this class owns what the GPU encode needs:
 *
 *  - the encoder pipeline (clean_texenc.comp: one invocation per 4x4 block of one mip
 *    level, read through a sampled UNORM view, blocks written to a buffer);
 *  - a device-local BLOCK SCRATCH RING (bounded, RenderVKTexEncodeScratchMB) the blocks
 *    land in before vkCmdCopyBufferToImage puts them into the BC image. Space is
 *    reserved when the upload is QUEUED (an image already handed to a bindless slot must
 *    be recorded by the next frame, so nothing is decided at record time) and freed when
 *    the frame that recorded it has finished (per frame-slot marks, like the staging ring);
 *  - a bounded, reused pool of RGBA8 TRANSIENT images (RenderVKTexEncodePoolMB): a fresh
 *    upload goes staging -> transient (+ the usual blit mip cascade) -> encoder -> BC
 *    image; the transient returns to the pool when its frame slot comes round;
 *  - per-frame-slot descriptor pools (one set per encode), the error-statistics records
 *    (PSNR log) and GPU timestamps (encode ms per frame).
 *
 * Every limit is checked when an encode is ADMITTED; a refused encode falls back to an
 * uncompressed upload, which the residency pass later re-encodes on the GPU.
 */
#ifndef LL_LLCLEANTEXENCVK_H
#define LL_LLCLEANTEXENCVK_H

#include "vk_device.h"
#include "vk_descpool.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

// How an image is stored (LLCleanRendererVK::SharedTex::enc).
enum LLTexEnc : uint8_t
{
    TEXENC_NONE = 0,   // R8G8B8A8 (uncompressed)
    TEXENC_BC1,        // BC1 RGB, 4 bpp (opaque colour, "Max savings")
    TEXENC_BC3,        // BC3, 8 bpp (alpha, "Max savings")
    TEXENC_BC4,        // BC4, 4 bpp (kernel present; no single-channel path uses it)
    TEXENC_BC5,        // BC5, 8 bpp (alpha-less tangent-space normal maps)
    TEXENC_BC7F,       // BC7 fast (mode 6 / 5), 8 bpp ("Balanced")
    TEXENC_BC7HQ,      // BC7 modes 6 / 5 / 1, 8 bpp ("High quality")
    TEXENC_COUNT
};

// Format helpers (shared by the streamer).
VkFormat texEncFormat(uint8_t enc, bool srgb);             // base format an encoding is stored in
VkFormat texViewFormat(VkFormat base, bool srgb);          // the sRGB / UNORM view of a base format
VkFormat texAltFormat(VkFormat base);                      // its mutable-format partner (or UNDEFINED)
VkFormat texUnormFormat(VkFormat base);                    // the UNORM variant (encoder input view)
bool     texFmtIsSrgb(VkFormat f);
bool     texFmtIsBC(VkFormat f);
uint64_t texLevelBytes(uint32_t w, uint32_t h, VkFormat f);
uint64_t texChainBytesFmt(uint32_t w, uint32_t h, VkFormat f, uint32_t levels = 32);
uint32_t texFmtCode(VkFormat f);                           // debug view code (clean_draw.frag)
const char* texEncName(uint8_t enc);
// The streamer's per-texture key from a texture UUID's 16 bytes (unsalted; the shared
// image key is key | 1). One definition: the persistent facts file stores UUIDs.
uint64_t texUuidKey(const uint8_t* id16);

class LLCleanTexEncVK
{
public:
    static constexpr uint32_t kSlots = 3;           // frame slots (= kTexDescSets)
    static constexpr uint32_t kMaxPerFrame = 192;   // encodes recorded per frame (descriptor sets)
    static constexpr uint32_t kStatRecs = kMaxPerFrame;

    // One queued encode (stored in LLCleanRendererVK::TexPendingItem).
    struct Job
    {
        uint8_t     enc = TEXENC_NONE;   // target encoding; with raw_decode: RGBA8 output
        bool        raw_decode = false;  // BC source -> RGBA8 (ENC_RAW)
        VkImageView src_view = VK_NULL_HANDLE;   // sampled UNORM view over the source
        bool        src_view_owned = false;      // a temporary view (retired after the frame)
        uint32_t    src_level = 0;       // source level feeding destination level 0
        uint64_t    scratch_off = 0;     // block scratch reservation
        uint64_t    scratch_bytes = 0;
        uint64_t    key = 0;             // shared-image key (stats attribution)
        // Adaptive format choice (#263): nonzero = deliver this encode's error measures as
        // a Result (takeResults) once its frame has finished. trial = the encode only
        // exists to measure a cheaper candidate (counted as the adaptive extra cost).
        uint32_t    tag = 0;
        bool        trial = false;
        // #840: an alpha-MASK image's 8-bit cutoff (the #804 mask variant's highest; 0 =
        // none): the encoder keeps every texel's alpha on its side of it (clean_texenc.comp,
        // ALPHA-MASK CUTOFF). Set when the encode is recorded.
        uint8_t     mask_cut = 0;
    };
    // Error measures of one tagged encode (level 0, whole blocks only), harvested when the
    // frame that recorded it has finished -- no extra wait or readback.
    struct Result
    {
        uint32_t tag = 0;
        uint8_t  enc = TEXENC_NONE;
        float    psnr = 0.f;          // whole image (dB, 99 = exact)
        float    smooth_psnr = 99.f;  // over the smooth (gradient) blocks only
        float    smooth_frac = 0.f;   // share of blocks that are smooth
        float    bad_frac = 0.f;      // share of blocks with RMS error > 10 levels
    };

    bool init(VkDeviceContext* ctx);
    void destroy();   // device idle
    bool ready() const { return mPipe != VK_NULL_HANDLE; }

    // ---- admission (queue time) ----
    // Start of a streamer call: refresh limits from settings, reset the per-call budget.
    void beginCall();
    // Would admit() succeed now? (pending cap, this call's texel budget, scratch space;
    // a refusal is counted in the stats unless count = false: a probe, e.g. a coverage-
    // chain swap waiting for room, must not steer the budget control.) Nothing changes
    // the answer until admit().
    bool check(uint32_t w, uint32_t h, uint32_t levels, VkFormat fmt, uint8_t enc, uint64_t* pad_out = nullptr,
               bool count = true);
    // Reserve block scratch for a w x h, `levels`-mip destination of `fmt` (or RGBA8 for
    // a raw decode); counts the encode against the call budget and the pending cap.
    bool admit(uint32_t w, uint32_t h, uint32_t levels, VkFormat fmt, uint8_t enc, Job& job);
    // Transient RGBA8 image (full chain, mutable sRGB/UNORM) from the pool or new; its
    // `view` member is the UNORM view the encoder samples. Empty on failure.
    GpuImage acquireTransient(uint32_t w, uint32_t h, bool srgb);
    // A transient handed out but not used (the upload was abandoned before queueing).
    void giveBackTransient(const GpuImage& img);
    uint32_t minDim() const { return mMinDim; }
    // BUDGET AUTO-SCALING: the fraction (1/16..1) of the configured caps
    // (RenderVKTexEncodeBudgetMP, RenderVKTexReencodePerPass) in force, steered by the
    // measured encode GPU time per frame (frames that encoded) toward
    // RenderVKTexEncodeTargetMs: down when over, up when well under while the budget is
    // what holds encodes back. 1 when the target is 0 (no adaptation).
    float budgetScale() const { return mBudgetScale; }
    // Compression off and nothing in flight: free the block scratch ring and the idle
    // transient pool (allocated again on the next encode). Returns the bytes freed (also the
    // VRAM pressure ladder's pool trim, #282: safe at any level while idle).
    uint64_t releaseIfIdle();

    // ---- recording (beginTextureFrame) ----
    // This frame slot's fence was waited: pooled transients / scratch / descriptor sets /
    // stats records of the frame that last used it are free; harvest its stats + timers.
    void beginFrame(uint32_t slot, uint64_t frame_no);
    // Queue the encode of `job` into `dst` (a fresh image: UNDEFINED -> every level
    // SHADER_READ_ONLY); flush() records every queued encode of this frame as one batch:
    // dispatches -> one barrier -> buffer-to-image copies -> one barrier, bracketed by
    // GPU timestamps (the encode ms in the VKMem line). The sources must already be
    // SHADER_READ_ONLY (transients: their upload + mip cascade recorded before flush).
    void queueRecord(const Job& job, VkImage dst, VkFormat dst_fmt, uint32_t w, uint32_t h, uint32_t levels);
    void flush(VkCommandBuffer cmd, uint32_t slot);
    // After everything pending was recorded into slot's frame.
    void endFrame(uint32_t slot);
    // A transient used by a recorded item: back to the pool when `slot` comes round.
    void releaseTransient(uint32_t slot, const GpuImage& img);
    uint32_t pendingEncodes() const { return mPending; }
    // Results of tagged encodes harvested since the last call (bounded; moved out).
    void takeResults(std::vector<Result>& out) { out.swap(mResults); mResults.clear(); }

    // ---- stats ----
    struct Totals
    {
        uint64_t encodes[TEXENC_COUNT] = {};    // completed encodes per target (raw decodes in [0])
        double   gpu_ms_window = 0.0, gpu_ms_max = 0.0;   // since the last statsLine
        uint32_t frames_window = 0, timed_window = 0;   // frames / frames that recorded encodes
        uint32_t refused_budget = 0, refused_scratch = 0, refused_pool = 0, refused_cap = 0;
        // adaptive trial encodes (the extra cost of the per-texture choice): count and
        // cost-weighted texels, against all encodes' cost-weighted texels
        uint64_t trial_encodes = 0, trial_cost = 0, all_cost = 0;
    };
    const Totals& totals() const { return mTot; }
    // VKMem fragment: encodes, GPU ms/frame, pool / scratch use, refusals.
    std::string statsLine();
    // VKTex PSNR distribution per target since the last call ("" when nothing new);
    // `name` maps a job key to a texture name for the worst-texture report.
    std::string psnrReport(const std::function<std::string(uint64_t)>& name);
    uint64_t poolBytes() const { return mPoolBytes; }
    uint64_t scratchBytes() const { return mScratch.size; }

private:
    struct PoolImg { GpuImage img; uint32_t w = 0, h = 0; bool srgb = true; uint64_t bytes = 0; };
    struct StatMeta { uint8_t enc = 0; uint32_t pixels = 0; uint64_t key = 0; uint32_t tag = 0; };
    std::vector<Result> mResults;
    static constexpr size_t kMaxResults = 4096;
    struct Queued { Job job; VkImage dst; VkFormat fmt; uint32_t w, h, levels; };
    std::vector<Queued> mQueued;
    void readStats(uint32_t slot);
    bool ensureScratch();
    void destroyPoolImg(PoolImg& p);

    VkDeviceContext* mCtx = nullptr;
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;
    VkPipelineLayout mLayout = VK_NULL_HANDLE;
    VkPipeline mPipe = VK_NULL_HANDLE;
    VkDescPool mPool[kSlots];
    bool mInitFailed = false;

    // block scratch ring (device-local)
    GpuBuffer mScratch{};
    uint64_t  mHeadV = 0, mTailV = 0, mMark[kSlots] = {};
    uint64_t  mScratchCap = 0;

    // transient pool
    std::vector<PoolImg> mFree;
    std::vector<PoolImg> mInFlight[kSlots];
    uint64_t mPoolBytes = 0;       // every pooled transient alive (free + handed out + in flight)
    uint64_t mPoolCap = 0;

    // stats + timers
    static constexpr uint32_t kStatWords = 8;   // uints per stats record (clean_texenc.comp)
    GpuBuffer mStats{};            // kSlots x kStatRecs x kStatWords uints, host-visible
    uint32_t  mStatUsed[kSlots] = {};
    std::vector<StatMeta> mStatMeta[kSlots];
    VkQueryPool mQuery = VK_NULL_HANDLE;
    bool mTimed[kSlots] = {};
    // #715 cache metrics (BC_ENCODE stage): jobs recorded into a slot's frame and their
    // RGBA source / block output bytes, reported with that frame's GPU time.
    uint32_t mSlotEncN[kSlots] = {};
    uint64_t mSlotEncIn[kSlots] = {}, mSlotEncOut[kSlots] = {};
    float mTsPeriod = 1.f;
    struct PsnrAcc { uint32_t hist[7] = {}; uint32_t n = 0; double worst = 1e9; uint64_t worst_key = 0; double sum = 0; };
    PsnrAcc mPsnr[TEXENC_COUNT];

    // limits / budget
    uint32_t mMinDim = 32;
    uint64_t mCallTexelBudget = 0, mCallTexels = 0;
    float    mBudgetMP = 4.f;      // the cap (RenderVKTexEncodeBudgetMP)
    float    mBudgetScale = 1.f, mTargetMs = 0.5f;
    double   mCtlMs[8] = {};       // encode GPU ms of the window's frames (timed frames only)
    uint32_t mCtlFrames = 0;
    uint32_t mCtlRefused0 = 0;     // refused_budget at the window start (was the budget binding?)
    double   mCtlQuietSince = 0.0; // steady seconds: no over-target window / refusal since
    uint32_t mCtlRefusedSeen = 0;
    double   mCtlNextRecover = 0.0;
    void     budgetControl(double frame_ms);
    void     budgetRecover();      // every frame: drift back toward the cap while quiet
    uint32_t mPending = 0;         // admitted, not yet recorded
    uint32_t mRecorded[kSlots] = {};
    Totals mTot;
};

// ---------------------------------------------------------------------------
// PERSISTENT TEXTURE FACTS (#263): what the streamer learned about a texture's pixels,
// kept across sessions by texture UUID (SL assets are immutable, so a UUID's content
// never changes):
//  - the Adaptive level's error MEASURES of the BC1 / BC3 candidate (not the verdict:
//    the threshold settings are applied when the measures are used, so tuning them never
//    invalidates the file) -- a texture measured once skips the dual encode for good;
//  - avatar texture (adaptive keeps BC7 HQ);
//  - constant colour verified at the asset's full size (stored as one texel at once);
//  - alpha channel present but unused at full size (encoded as opaque from the first
//    upload instead of after a re-encode).
// File: <cache>/vk_texcomp_decisions.bin. Header (magic, format + encoder versions,
// record size, count, CRC); anything that doesn't match is discarded and rewritten, never
// half-used. Loaded on a worker thread, saved every few minutes when changed (worker,
// write-temp-then-rename) and at shutdown. Bounded: kMaxEntries, least-recently-used out.
// Clear Cache deletes the file (LLAppViewer::purgeCache).
class LLCleanTexFacts
{
public:
    enum : uint8_t
    {
        F_MEAS = 1,           // the adaptive measures below are set
        F_MEAS_ALPHA = 2,     // ... measured as BC3 (the texture had alpha) / else BC1
        F_MEAS_VS_BC7 = 4,    // ... measured against BC7 HQ pixels (needs +1 dB)
        F_AVATAR = 8,
        F_CONST_FULL = 16,    // one colour (const_rgba) at full_w x full_h
        F_ALPHA_UNUSED = 32,  // 4 channels, alpha 255 everywhere at full size
    };
    struct Rec
    {
        uint8_t  id[16] = {};                         // texture UUID
        uint16_t psnr = 0, smooth_psnr = 0, ref_psnr = 0;   // dB x 100
        uint16_t smooth_frac = 0, bad_frac = 0;       // x 65535
        uint8_t  flags = 0, pad = 0;
        uint32_t const_rgba = 0;
        uint16_t full_w = 0, full_h = 0;
        uint32_t stamp = 0;                           // last use (LRU order, across sessions)
    };
    static constexpr size_t   kMaxEntries = 262144;   // ~11 MB file / ~20 MB in memory at the cap
    // Bump when clean_texenc.comp's encoders or error measures change: old measures go.
    static constexpr uint32_t kEncoderVersion = 1;

    ~LLCleanTexFacts();
    // Start reading `path` on a worker thread ("" = persistence off: memory only).
    void startLoad(const std::string& path);
    // Main thread, each texture pass: merge a finished load; start a periodic save.
    void poll(double now_s);
    // Shutdown: wait for workers, write the file if anything changed.
    void shutdown();
    const Rec* find(uint64_t key) const;
    // Find-or-insert (bounded); stamps it as used and marks the store changed.
    Rec& edit(uint64_t key, const uint8_t* id16);
    void touch(uint64_t key);
    size_t size() const { return mMap.size(); }
    bool loaded() const { return mMerged; }
    size_t loadedCount() const { return mLoadedCount; }
    const std::string& state() const { return mState; }

    static uint16_t qdb(float db) { return (uint16_t)std::min(65535.f, std::max(0.f, db * 100.f + 0.5f)); }
    static float    dqdb(uint16_t q) { return (float)q / 100.f; }
    static uint16_t qfr(float f) { return (uint16_t)std::min(65535.f, std::max(0.f, f * 65535.f + 0.5f)); }
    static float    dqfr(uint16_t q) { return (float)q / 65535.f; }

private:
    void evict();
    bool writeFile(const std::vector<Rec>& recs) const;   // any thread
    std::unordered_map<uint64_t, Rec> mMap;
    uint32_t mClock = 1;
    bool     mDirty = false;
    double   mNextSave = 0.0;
    std::string mPath;
    std::string mState = "off";
    bool     mMerged = false;
    size_t   mLoadedCount = 0;
    // worker load
    std::thread mLoadThread;
    std::atomic<bool> mLoadDone{false};
    std::unordered_map<uint64_t, Rec> mLoadMap;   // built on the worker (no main-thread rehash of 262k)
    uint32_t mLoadMaxStamp = 0;
    std::string mLoadState;
    // worker save
    std::thread mSaveThread;
    std::atomic<bool> mSaveBusy{false};
};

#endif
