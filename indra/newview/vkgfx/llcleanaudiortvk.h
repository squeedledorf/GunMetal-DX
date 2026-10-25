/**
 * @file llcleanaudiortvk.h
 * @brief Acoustics probe (#683 phase 0): listener room rays + per-source occlusion rays
 *        against the Vulkan renderer's scene TLAS.
 *
 * One tiny compute dispatch per frame (clean_audio_rt.comp, 64 lanes; + one 64-lane
 * workgroup per 64 #857 / #858 path queries listed, at most 2), only while AudioAcoustics asks
 * for it (LLVKWorldFrame::acoustics.valid) and this frame's TLAS is valid and warm (ray
 * tracing on: RenderVKRayTracing + VK_KHR_ray_query). #858 diffraction shadow rays (room
 * hit -> occluded source) ride on the room frames; see LLVKAcousticsRequest.
 *
 *  - REQUEST (setRequest, before recordWorld): listener + up to 32 sound sources, agent
 *    space, from newview/llcleanacoustics.cpp.
 *  - RECORD (record, in recordWorld after the TLAS is released to compute readers): the
 *    sources are written into this frame slot's record of a host-visible buffer, the
 *    dispatch writes the answers into the same record (reached by device address).
 *  - HARVEST (beginFrame, when the frame slot comes round: its fence was waited) -> the
 *    newest answer (latest), serial-checked so a skipped or stale record is never read.
 *    Per room ray (alpha-mask foliage included, alpha-tested like the raster's cut-out; with
 *    AudioAcousticsAlphaBlocks visible alpha-blend too -- anything not fully transparent):
 *    distance, z, the hit's instance id (the CPU resolves the prim's physical material), a
 *    kind (foliage / glass), a render-material hardness hint and a geometric clutter score.
 *    Per source: the opaque blocking distance and (alpha blocks) the nearest visible blend /
 *    alpha-mask surface crossed.
 *
 * Pipeline sets: 0 = the RT consumer set (the full clean_rt_common.glsl set: TLAS,
 * instances, materials, geometry for the candidate alpha test), 1 = the renderer's
 * draw set 1 (bindless textures: one smallest-mip read of a hit's MR / specular map).
 *
 * #683 PHASE 1 BOUNCE PASS (recordBounce): a second dispatch of the same pipeline (push bit 2)
 * on its own frames -- multi-bounce energy tracing from the listener and a few sources, one
 * partial record per 64-ray workgroup (appended to the slot record: AcSlot::bpart), summed per
 * set at harvest into LLVKAcousticsBounce (latestBounce). Default 512 listener rays x 6 bounces
 * (+ a shadow ray per bounce) every 3rd frame. #683 EARLY GAIN: source sets whose receiver is the
 * LISTENER (AcBounceSet cfg.z bit 0), with their own bounce cap and receiver radius (cfg.z bits).
 *
 * #930 LYING SNOW (AudioAcousticsSnowAbsorb; LLVKAcousticsRequest::snow_strength > 0): while the
 * weather publishes its snow depth grids (LLCleanWeatherVK::acousticsSnow), both dispatches run the
 * AC_SNOW variant of the pipeline (kWorldAudioRtSnowSpv) with a set 2 of this module's own (one per
 * frame slot, written once per recording: the inner grid + its base + the outer ring, storage images,
 * read only) and the grids' frames in the slot record. One column lookup per committed hit; the
 * answers carry each room ray's / RAY query's snow share and compaction, the bounce pass blends its
 * hit classes itself. No snow (or the strength 0, or the weather off): the plain pipeline, exactly
 * as before.
 *
 * Wanting the probe makes the renderer keep the TLAS built (rtConsumerActive) even when no
 * other RT consumer is on -- that BLAS / TLAS upkeep, not the ~100 rays, is the real cost.
 */
#ifndef LL_LLCLEANAUDIORTVK_H
#define LL_LLCLEANAUDIORTVK_H

#include "vk_device.h"
#include "vk_descpool.h"
#include "llvulkancontext.h"   // LLVKAcousticsRequest / LLVKAcousticsResult

#include <cstdint>

class LLCleanRendererVK;

class LLCleanAudioRTVK
{
public:
    explicit LLCleanAudioRTVK(LLCleanRendererVK& r);
    ~LLCleanAudioRTVK() = default;

    // Device must be idle; before the RT module (the pipeline layout names its consumer layout).
    void destroy(VkDevice dev);

    // This frame's request (LLVKWorldFrame::acoustics).
    void setRequest(const LLVKAcousticsRequest& req) { mReq = req; }
    // A request is pending this frame and nothing latched the probe off.
    bool wanted() const { return mReq.valid && !mFailed; }
    // Top of recordWorld (this frame slot's fence waited): harvest the slot's previous answer.
    void beginFrame();
    // After the TLAS build / release (rtSet = this frame's RT consumer set): the dispatch.
    // No-op unless wanted().
    void record(VkCommandBuffer cmd, VkDescriptorSetLayout rtLayout, VkDescriptorSet rtSet);
    // #683 phase 1 BOUNCE PASS (right after record, same sets): the multi-bounce dispatch on its
    // frames (LLVKAcousticsRequest::bounce, every room_interval-th frame, one after the room
    // rays). true = dispatched (the caller's c_audio_bounce timer mark).
    bool recordBounce(VkCommandBuffer cmd, VkDescriptorSetLayout rtLayout, VkDescriptorSet rtSet);

    bool latest(LLVKAcousticsResult& out) const;
    // The newest bounce answer, copied only when its serial differs from `seen`.
    bool latestBounce(LLVKAcousticsBounce& out, uint32_t seen) const;
    // The request serial of the last dispatched bounce pass (0: none).
    uint32_t lastBounceDispatch() const { return mLastBounceDispatch; }

private:
    bool ensure(VkDescriptorSetLayout rtLayout);
    uint32_t slotIndex() const;
    // #930: the AC_SNOW pipeline + its set layout / pool (lazily, on the first snowy frame; a
    // failure latches only the snow off).
    bool ensureSnow();
    // #930: this recording's snow decision (once per recording: the slot's set is written then),
    // and the record's snow inputs (sfr / sprm) filled when it is on. true = run the AC_SNOW pipeline.
    bool snowFor(uint32_t slot, uint8_t* rec);

    static constexpr uint32_t kSlots = 3;   // the renderer's frame slots

    LLCleanRendererVK& mR;
    VkDeviceContext* mCtx = nullptr;
    VkPipelineLayout mLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout mLayoutRt = VK_NULL_HANDLE;   // the RT consumer layout mLayout was made with
    VkPipeline mPipe = VK_NULL_HANDLE;
    GpuBuffer mBuf;                                       // kSlots AcSlot records (host-visible, device address)
    bool mFailed = false;                                 // latched: allocation / pipeline creation failed
    // #930 lying snow (AC_SNOW variant): layout {RT consumer, draw set 1, mDslSnow}.
    VkDescriptorSetLayout mDslSnow = VK_NULL_HANDLE;
    VkDescPool            mSnowPool;
    VkDescriptorSet       mSnowSet[kSlots] = {};          // one per frame slot
    VkPipelineLayout      mLayoutSnow = VK_NULL_HANDLE;
    VkPipeline            mPipeSnow = VK_NULL_HANDLE;
    bool                  mSnowFailed = false;            // latched: the snow variant could not be created
    uint32_t              mSnowRetry = 0;                 // mFrame before which a failed (out-of-memory) set allocation is not retried
    uint32_t              mSnowFrame = 0xFFFFFFFFu;       // mFrame of the last snow decision
    bool                  mSnowNow = false;               // ... its answer
    float                 mSnowIn[5][4] = {};             // ... the record's sfr[4] + sprm

    LLVKAcousticsRequest mReq;
    struct SlotState
    {
        bool     pending = false;
        uint32_t frame = 0;          // mFrame when recorded
        bool     room = false;       // this record traced the room rays (and the #858 shadow rays)
        uint32_t queries = 0;        // #857 / #858 path queries this record ran
        bool     snow = false;       // #930: it ran the AC_SNOW variant (rsnow / qsnow written)
        LLVKAcousticsRequest req;    // what it was asked
        // #683 phase 1 bounce pass (its own record region; harvested with the slot)
        bool     bounce_pending = false;
        uint32_t bounce_serial = 0;
        uint32_t bounce_frame = 0;
        uint32_t bounce_sets = 0;
        uint32_t bounce_wg = 0;
        uint32_t bounce_wg0[LLVKAcousticsRequest::kMaxBounceSets] = {};   // each set's first workgroup
        uint32_t bounce_nwg[LLVKAcousticsRequest::kMaxBounceSets] = {};   // ... and its count
        uint32_t bounce_src[LLVKAcousticsRequest::kMaxBounceSets] = {};   // ... and its source index (kNone: listener)
        float    bounce_origin[LLVKAcousticsRequest::kMaxBounceSets][3] = {};
        float    bounce_recv[LLVKAcousticsRequest::kMaxBounceSets][3] = {};    // ... its receiver
        float    bounce_set_rr[LLVKAcousticsRequest::kMaxBounceSets] = {};     // ... its receiver radius
        uint32_t bounce_set_max[LLVKAcousticsRequest::kMaxBounceSets] = {};    // ... its bounce cap
        bool     bounce_to_l[LLVKAcousticsRequest::kMaxBounceSets] = {};       // ... an early-gain set
        uint32_t bounce_max = 0;
        float    bounce_range = 0.f, bounce_rr = 1.f, bounce_speed = 343.f;
        float    bounce_air[3] = {};
    };
    SlotState mSlot[kSlots];
    uint32_t mFrame = 0;             // record() calls (frames_old)
    uint32_t mLastBounceDispatch = 0;   // request serial of the last bounce dispatch
    LLVKAcousticsResult mLatest;
    LLVKAcousticsBounce mLatestBounce;   // #683 phase 1
    void harvestBounce(uint32_t slot, SlotState& st);
};

#endif // LL_LLCLEANAUDIORTVK_H
