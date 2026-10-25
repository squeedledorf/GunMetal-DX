/**
 * @file gmdxreplayvk.cpp
 * @brief GunMetal-DX instant replay: the last seconds of the frame, encoded on the GPU.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

// <GMDX> See gmdxreplayvk.h. Per captured frame, in slot s of kSlots:
//
//  GRAPHICS (recorded into the frame's last command buffer, recordCapture):
//    finished image --copy--> mCap[s] (its UNORM twin, window size)
//    mCap[s] --gmdx_replay_nv12.comp (scale + BT.709)--> mY[s] (R8) + mUV[s] (RG8, half size)
//    mY / mUV --copy--> planes 0 / 1 of mSrc[s] (the encoder's 4:2:0 input), -> VIDEO_ENCODE_SRC
//  then an empty graphics submission signals mGfxTl = v (submit()).
//  ENCODE (the video encode queue): waits mGfxTl >= v, encodes mSrc[s] into mBits[s] with a
//    feedback query (offset + bytes written), signals mEncTl = v.
//  poll() reads finished slots' bytes into the replay buffer and frees them.
//
// DPB: two slots of one 2-layer image, used alternately: each frame is reconstructed into one
// and references the other (the previous frame). An IDR every gop frames resets the chain.

#include "gmdxreplayvk.h"

#include <vk_video/vulkan_video_codec_h264std.h>
#include <vk_video/vulkan_video_codec_h264std_encode.h>

#ifdef _WIN32
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <cmath>
#include <deque>
#include <mutex>
#include <thread>
#include <cstdio>
#include <cstring>

namespace
{
constexpr uint32_t kSlots = 4;            // frames in flight between capture and harvest
constexpr uint32_t kDpbSlots = 2;
constexpr VkDeviceSize kMinBitstream = 4u << 20;

// vkCmdPipelineBarrier2 / vkQueueSubmit2: core on 1.3 devices, the KHR names on 1.2 ones
// (fetched in init; the loader's core exports are null on a 1.2 device).
PFN_vkCmdPipelineBarrier2 g_barrier2 = nullptr;
PFN_vkQueueSubmit2 g_submit2 = nullptr;
void fnBarrier2(VkCommandBuffer c, const VkDependencyInfo* d) { g_barrier2(c, d); }
VkResult fnSubmit2(VkQueue q, uint32_t n, const VkSubmitInfo2* s, VkFence f) { return g_submit2(q, n, s, f); }

struct Fn
{
    PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR getCaps = nullptr;
    PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR getFormats = nullptr;
    PFN_vkCreateVideoSessionKHR createSession = nullptr;
    PFN_vkDestroyVideoSessionKHR destroySession = nullptr;
    PFN_vkGetVideoSessionMemoryRequirementsKHR sessionMemReqs = nullptr;
    PFN_vkBindVideoSessionMemoryKHR bindSessionMem = nullptr;
    PFN_vkCreateVideoSessionParametersKHR createParams = nullptr;
    PFN_vkDestroyVideoSessionParametersKHR destroyParams = nullptr;
    PFN_vkGetEncodedVideoSessionParametersKHR getEncodedParams = nullptr;
    PFN_vkCmdBeginVideoCodingKHR beginCoding = nullptr;
    PFN_vkCmdEndVideoCodingKHR endCoding = nullptr;
    PFN_vkCmdControlVideoCodingKHR controlCoding = nullptr;
    PFN_vkCmdEncodeVideoKHR encode = nullptr;
};

struct Packet
{
    std::vector<uint8_t> annexB;   // as the encoder wrote it (start codes)
    double t = 0.0;                // capture time, seconds
    bool idr = false;
};
using PacketPtr = std::shared_ptr<const Packet>;

struct Image
{
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

uint32_t alignUp(uint32_t v, uint32_t a) { return a ? (v + a - 1) / a * a : v; }

VkFormat unormTwin(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    default: return f;
    }
}

// ---- MP4 (ISO BMFF) writer: one H.264 video track, samples in one chunk ----
struct Box
{
    std::vector<uint8_t> b;
    void u8(uint8_t v) { b.push_back(v); }
    void u16(uint32_t v) { u8((uint8_t)(v >> 8)); u8((uint8_t)v); }
    void u32(uint32_t v) { u16(v >> 16); u16(v & 0xFFFF); }
    void fourcc(const char* c) { b.insert(b.end(), c, c + 4); }
    void bytes(const uint8_t* p, size_t n) { b.insert(b.end(), p, p + n); }
    void zeros(size_t n) { b.insert(b.end(), n, 0); }
    void full(uint8_t ver, uint32_t flags) { u8(ver); u8((uint8_t)(flags >> 16)); u16(flags & 0xFFFF); }
    void matrix() { const uint32_t m[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000}; for (uint32_t v : m) u32(v); }
    void child(const char* type, const Box& c)
    {
        u32((uint32_t)c.b.size() + 8);
        fourcc(type);
        b.insert(b.end(), c.b.begin(), c.b.end());
    }
};

// Annex B -> the NAL units (without start codes).
std::vector<std::pair<const uint8_t*, size_t>> splitNals(const std::vector<uint8_t>& s)
{
    std::vector<std::pair<const uint8_t*, size_t>> out;
    const size_t n = s.size();
    size_t i = 0, start = SIZE_MAX;
    while (i + 2 < n)
    {
        if (s[i] == 0 && s[i + 1] == 0 && s[i + 2] == 1)
        {
            if (start != SIZE_MAX)
            {
                size_t end = i;
                while (end > start && s[end - 1] == 0) --end;   // trailing zero of a 4-byte start code
                out.emplace_back(s.data() + start, end - start);
            }
            i += 3;
            start = i;
            continue;
        }
        ++i;
    }
    if (start != SIZE_MAX && start < n) out.emplace_back(s.data() + start, n - start);
    return out;
}

FILE* openUtf8(const std::string& path)
{
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n : 1, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    return _wfopen(w.c_str(), L"wb");
#else
    return std::fopen(path.c_str(), "wb");
#endif
}

void removeUtf8(const std::string& path)
{
#ifdef _WIN32
    const int n = MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n : 1, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, path.c_str(), -1, &w[0], n);
    _wremove(w.c_str());
#else
    std::remove(path.c_str());
#endif
}

// The data-reference and media-info boxes both tracks share
void dinfBox(Box& minf)
{
    Box dinf, dref, url;
    url.full(0, 1);
    dref.full(0, 0); dref.u32(1); dref.child("url ", url);
    dinf.child("dref", dref);
    minf.child("dinf", dinf);
}

void hdlrBox(Box& mdia, const char* type, const char* name)
{
    Box hdlr;
    hdlr.full(0, 0); hdlr.u32(0); hdlr.fourcc(type); hdlr.zeros(12);
    hdlr.bytes((const uint8_t*)name, std::strlen(name) + 1);
    mdia.child("hdlr", hdlr);
}

void tkhdBox(Box& trak, uint32_t id, uint32_t durMs, bool audio, uint32_t width, uint32_t height)
{
    Box tkhd;
    tkhd.full(0, 3); tkhd.u32(0); tkhd.u32(0); tkhd.u32(id); tkhd.u32(0); tkhd.u32(durMs); tkhd.zeros(8);
    tkhd.u16(0); tkhd.u16(0); tkhd.u16(audio ? 0x0100 : 0); tkhd.u16(0); tkhd.matrix();
    tkhd.u32(width << 16); tkhd.u32(height << 16);
    trak.child("tkhd", tkhd);
}

bool writeMp4(const std::string& path, const std::vector<uint8_t>& sps, const std::vector<uint8_t>& pps,
              uint32_t width, uint32_t height, uint32_t fps, const std::vector<PacketPtr>& packets,
              const GMDXReplayAudioClip& audio, std::string& msg)
{
    if (packets.empty() || sps.size() < 4 || pps.empty()) { msg = "nothing to write"; return false; }

    // durations (90 kHz) from the capture times; the last repeats the average
    constexpr uint32_t kTs = 90000;
    std::vector<uint32_t> durs;
    for (size_t k = 0; k + 1 < packets.size(); ++k)
        durs.push_back((uint32_t)std::max(1.0, std::round((packets[k + 1]->t - packets[k]->t) * kTs)));
    uint64_t total = 0;
    for (uint32_t d : durs) total += d;
    durs.push_back(durs.empty() ? kTs / std::max(1u, fps) : (uint32_t)(total / durs.size()));
    total += durs.back();
    const uint32_t durMs = (uint32_t)(total * 1000 / kTs);

    // audio: the 20 ms packets inside the video's span; lead = where the first starts, from the video's start
    constexpr uint32_t kAr = 48000, kAf = 960;
    const double t0 = packets.front()->t, tEnd = t0 + total / (double)kTs;
    size_t a0 = 0, a1 = 0;
    if (!audio.packets.empty())
    {
        a0 = (size_t)std::max(0.0, std::ceil((t0 - 0.01 - audio.startSec) * 50.0));
        a1 = audio.packets.size();
        while (a1 > a0 && audio.startSec + (a1 - 1) / 50.0 >= tEnd) --a1;
    }
    const bool hasAudio = a1 > a0;
    const double lead = hasAudio ? audio.startSec + a0 / 50.0 - t0 : 0.0;

    FILE* f = openUtf8(path);
    if (!f) { msg = "can't create " + path; return false; }
    bool ok = true;
    auto put = [&](const std::vector<uint8_t>& v) { ok = ok && std::fwrite(v.data(), 1, v.size(), f) == v.size(); };

    Box ftyp;
    ftyp.fourcc("isom"); ftyp.u32(0x200); ftyp.fourcc("isom"); ftyp.fourcc("iso2"); ftyp.fourcc("avc1"); ftyp.fourcc("mp41");
    Box file;
    file.child("ftyp", ftyp);
    put(file.b);
    const uint64_t mdatStart = file.b.size();

    // mdat: the video chunk (length-prefixed slice NALs; parameter sets live in avcC), then the audio chunk
    std::vector<uint32_t> sizes, aSizes;
    std::vector<uint32_t> syncs;
    std::vector<uint8_t> hdr = {0, 0, 0, 0, 'm', 'd', 'a', 't'};
    put(hdr);
    uint64_t mdatSize = 8;
    for (size_t k = 0; k < packets.size(); ++k)
    {
        Box s;
        for (const auto& nal : splitNals(packets[k]->annexB))
        {
            const uint8_t type = nal.first[0] & 0x1F;
            if (type == 7 || type == 8 || type == 9) continue;   // SPS / PPS / AUD
            s.u32((uint32_t)nal.second);
            s.bytes(nal.first, nal.second);
        }
        put(s.b);
        sizes.push_back((uint32_t)s.b.size());
        mdatSize += s.b.size();
        if (packets[k]->idr) syncs.push_back((uint32_t)k + 1);
    }
    const uint64_t audioChunk = mdatStart + mdatSize;
    for (size_t k = a0; k < a1; ++k)
    {
        put(audio.packets[k]);
        aSizes.push_back((uint32_t)audio.packets[k].size());
        mdatSize += audio.packets[k].size();
    }

    Box stsd, avc1, avcC, colr, stts, stss, stsc, stsz, stco;
    avcC.u8(1); avcC.u8(sps[1]); avcC.u8(sps[2]); avcC.u8(sps[3]); avcC.u8(0xFF); avcC.u8(0xE1);
    avcC.u16((uint32_t)sps.size()); avcC.bytes(sps.data(), sps.size());
    avcC.u8(1); avcC.u16((uint32_t)pps.size()); avcC.bytes(pps.data(), pps.size());
    colr.fourcc("nclx"); colr.u16(1); colr.u16(1); colr.u16(1); colr.u8(0);   // BT.709, limited range
    avc1.zeros(6); avc1.u16(1); avc1.zeros(16); avc1.u16(width); avc1.u16(height);
    avc1.u32(0x00480000); avc1.u32(0x00480000); avc1.u32(0); avc1.u16(1);
    {
        uint8_t name[32] = {};
        const char* n = "GunMetal-DX";
        name[0] = (uint8_t)std::strlen(n);
        std::memcpy(name + 1, n, name[0]);
        avc1.bytes(name, 32);
    }
    avc1.u16(0x18); avc1.u16(0xFFFF);
    avc1.child("avcC", avcC);
    avc1.child("colr", colr);
    stsd.full(0, 0); stsd.u32(1); stsd.child("avc1", avc1);

    std::vector<std::pair<uint32_t, uint32_t>> runs;
    for (uint32_t d : durs)
    {
        if (!runs.empty() && runs.back().second == d) ++runs.back().first;
        else runs.emplace_back(1, d);
    }
    stts.full(0, 0); stts.u32((uint32_t)runs.size());
    for (auto& r : runs) { stts.u32(r.first); stts.u32(r.second); }
    stss.full(0, 0); stss.u32((uint32_t)syncs.size());
    for (uint32_t s : syncs) stss.u32(s);
    stsc.full(0, 0); stsc.u32(1); stsc.u32(1); stsc.u32((uint32_t)sizes.size()); stsc.u32(1);
    stsz.full(0, 0); stsz.u32(0); stsz.u32((uint32_t)sizes.size());
    for (uint32_t s : sizes) stsz.u32(s);
    stco.full(0, 0); stco.u32(1); stco.u32((uint32_t)(mdatStart + 8));

    Box stbl, minf, vmhd, mdia, mdhd, trak, mvhd, moov;
    stbl.child("stsd", stsd); stbl.child("stts", stts); stbl.child("stss", stss);
    stbl.child("stsc", stsc); stbl.child("stsz", stsz); stbl.child("stco", stco);
    vmhd.full(0, 1); vmhd.zeros(8);
    minf.child("vmhd", vmhd); dinfBox(minf); minf.child("stbl", stbl);
    mdhd.full(0, 0); mdhd.u32(0); mdhd.u32(0); mdhd.u32(kTs); mdhd.u32((uint32_t)total); mdhd.u16(0x55C4); mdhd.u16(0);
    mdia.child("mdhd", mdhd); hdlrBox(mdia, "vide", "GunMetal-DX replay"); mdia.child("minf", minf);
    tkhdBox(trak, 1, durMs, false, width, height);
    trak.child("mdia", mdia);

    // the Opus track (ISO/IEC 14496-12 + the Opus-in-ISOBMFF mapping): an edit list places it on
    // the video's timeline and skips the encoder's pre-roll
    Box atrak;
    uint32_t aDurMs = 0;
    if (hasAudio)
    {
        const uint32_t n = (uint32_t)(a1 - a0);
        const uint64_t samples = (uint64_t)n * kAf;
        const uint32_t leadMs = lead > 0.0005 ? (uint32_t)std::lround(lead * 1000.0) : 0;
        const uint32_t skip = audio.preSkip + (lead < 0.0 ? (uint32_t)std::lround(-lead * kAr) : 0);
        const uint32_t playMs = (uint32_t)((samples > skip ? samples - skip : 0) * 1000 / kAr);
        aDurMs = leadMs + playMs;

        Box dOps, opus, astsd, astts, astsc, astsz, astco, astbl, smhd, aminf, amdhd, amdia, elst, edts;
        dOps.u8(0); dOps.u8((uint8_t)audio.channels); dOps.u16(audio.preSkip); dOps.u32(kAr); dOps.u16(0); dOps.u8(0);
        opus.zeros(6); opus.u16(1); opus.zeros(8); opus.u16(audio.channels); opus.u16(16); opus.u16(0); opus.u16(0);
        opus.u32(kAr << 16);
        opus.child("dOps", dOps);
        astsd.full(0, 0); astsd.u32(1); astsd.child("Opus", opus);
        astts.full(0, 0); astts.u32(1); astts.u32(n); astts.u32(kAf);
        astsc.full(0, 0); astsc.u32(1); astsc.u32(1); astsc.u32(n); astsc.u32(1);
        astsz.full(0, 0); astsz.u32(0); astsz.u32(n);
        for (uint32_t s : aSizes) astsz.u32(s);
        astco.full(0, 0); astco.u32(1); astco.u32((uint32_t)audioChunk);
        astbl.child("stsd", astsd); astbl.child("stts", astts); astbl.child("stsc", astsc);
        astbl.child("stsz", astsz); astbl.child("stco", astco);
        smhd.full(0, 0); smhd.u16(0); smhd.u16(0);
        aminf.child("smhd", smhd); dinfBox(aminf); aminf.child("stbl", astbl);
        amdhd.full(0, 0); amdhd.u32(0); amdhd.u32(0); amdhd.u32(kAr); amdhd.u32((uint32_t)samples); amdhd.u16(0x55C4); amdhd.u16(0);
        amdia.child("mdhd", amdhd); hdlrBox(amdia, "soun", "GunMetal-DX replay audio"); amdia.child("minf", aminf);
        elst.full(0, 0); elst.u32(leadMs ? 2 : 1);
        if (leadMs) { elst.u32(leadMs); elst.u32(0xFFFFFFFFu); elst.u32(0x00010000); }   // empty edit: silence first
        elst.u32(playMs); elst.u32(skip); elst.u32(0x00010000);
        edts.child("elst", elst);
        tkhdBox(atrak, 2, aDurMs, true, 0, 0);
        atrak.child("edts", edts);
        atrak.child("mdia", amdia);
    }

    mvhd.full(0, 0); mvhd.u32(0); mvhd.u32(0); mvhd.u32(1000); mvhd.u32(std::max(durMs, aDurMs)); mvhd.u32(0x00010000); mvhd.u16(0x0100);
    mvhd.zeros(10); mvhd.matrix(); mvhd.zeros(24); mvhd.u32(hasAudio ? 3 : 2);
    moov.child("mvhd", mvhd); moov.child("trak", trak);
    if (hasAudio) moov.child("trak", atrak);
    Box tail;
    tail.child("moov", moov);
    put(tail.b);

    // patch the mdat size
    if (ok && mdatSize < 0xFFFFFFFFull)
    {
        const uint8_t sz[4] = {(uint8_t)(mdatSize >> 24), (uint8_t)(mdatSize >> 16), (uint8_t)(mdatSize >> 8), (uint8_t)mdatSize};
        ok = std::fseek(f, (long)mdatStart, SEEK_SET) == 0 && std::fwrite(sz, 1, 4, f) == 4;
    }
    else ok = false;
    ok = (std::fclose(f) == 0) && ok;
    if (!ok) { msg = "write failed: " + path; removeUtf8(path); return false; }
    char buf[160];
    std::snprintf(buf, sizeof(buf), "%.1f s, %ux%u%s, %.1f MB", total / (double)kTs, width, height,
                  hasAudio ? " + audio" : "", (mdatSize + tail.b.size()) / 1048576.0);
    msg = buf;
    return true;
}
} // namespace

// ---------------------------------------------------------------------------------------------

struct GMDXReplayEncoder::Impl
{
    Device dev;
    Fn fn;
    VkPhysicalDeviceMemoryProperties memProps{};

    // profile (chained into every video object's create info)
    VkVideoEncodeH264ProfileInfoKHR h264Profile{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR};
    VkVideoProfileInfoKHR profile{VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR};
    VkVideoProfileListInfoKHR profileList{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
    VkVideoCapabilitiesKHR caps{VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR};
    VkVideoEncodeCapabilitiesKHR encCaps{VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR};
    VkVideoEncodeH264CapabilitiesKHR h264Caps{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR};
    VkFormat srcFormat = VK_FORMAT_UNDEFINED, dpbFormat = VK_FORMAT_UNDEFINED;
    VkVideoEncodeRateControlModeFlagBitsKHR rcMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR;

    // conversion
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet sets[kSlots] = {};

    // sync
    VkSemaphore gfxTl = VK_NULL_HANDLE, encTl = VK_NULL_HANDLE;
    uint64_t nextValue = 0;
    VkCommandPool encPool = VK_NULL_HANDLE;
    VkCommandBuffer encCmd[kSlots] = {};
    VkQueryPool queries = VK_NULL_HANDLE;

    // per configuration (size)
    uint32_t srcW = 0, srcH = 0;            // finished image size
    VkFormat capFormat = VK_FORMAT_UNDEFINED;
    uint32_t picW = 0, picH = 0, codedW = 0, codedH = 0;
    uint32_t fps = 60, bitrateKbps = 0, gop = 60, maxH = 0;
    Image cap[kSlots], yImg[kSlots], uvImg[kSlots], src[kSlots], dpb;
    VkImageView capView[kSlots] = {}, yView[kSlots] = {}, uvView[kSlots] = {}, srcView[kSlots] = {}, dpbView[kDpbSlots] = {};
    VkBuffer bits[kSlots] = {};
    VkDeviceMemory bitsMem[kSlots] = {};
    uint8_t* bitsMap[kSlots] = {};
    VkDeviceSize bitsSize = 0;
    VkVideoSessionKHR session = VK_NULL_HANDLE;
    std::vector<VkDeviceMemory> sessionMem;
    VkVideoSessionParametersKHR params = VK_NULL_HANDLE;
    std::vector<uint8_t> spsNal, ppsNal;
    bool configured = false;
    bool sessionReset = false;   // rate control set (the first encode resets the session)
    bool dpbReady = false;

    // stream state
    uint32_t frameInGop = 0;     // 0 = next is an IDR
    uint32_t encodes = 0;        // pictures sent since the last reset (DPB slot = encodes % 2)
    uint32_t idrId = 0;

    enum SlotState { Free, Recorded, Encoding };
    struct Slot { SlotState state = Free; uint64_t value = 0; double t = 0.0; bool idr = false; };
    Slot slots[kSlots];
    int recordedSlot = -1;
    double lastCapture = -1.0;
    // reconfiguring waits for the size / settings to hold still (a window drag must not
    // rebuild everything every frame); a failed configure retries later
    uint32_t wantW = 0, wantH = 0;
    double wantSince = 0.0, retryAt = 0.0;
    std::string error;            // for the viewer to report once (takeError)
    bool skipUntilIdr = false;    // a lost picture: its P successors are dropped
    int64_t lastTick = INT64_MIN; // sample times sit on the 1/fps grid, strictly increasing
    double lastIdrT = -1e9;       // an IDR at least every second of capture time (low frame rates too)

    // replay buffer
    std::deque<PacketPtr> ring;
    uint32_t keepSeconds = 30;
    uint64_t dropped = 0, encoded = 0;

    // saving
    std::thread saver;
    std::atomic<bool> saving{false};
    std::mutex resultMutex;
    bool haveResult = false, resultOk = false;
    std::string resultMsg;

    uint32_t memType(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer = 0) const
    {
        for (int pass = 0; pass < 2; ++pass)
        {
            const VkMemoryPropertyFlags need = pass == 0 ? (want | prefer) : want;
            for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
                if ((bits & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & need) == need) return i;
        }
        return ~0u;
    }

    bool makeImage(Image& out, const VkImageCreateInfo& ci, bool video)
    {
        VkImageCreateInfo c = ci;
        VkVideoProfileListInfoKHR pl = profileList;
        if (video) { pl.pNext = c.pNext; c.pNext = &pl; }
        if (vkCreateImage(dev.device, &c, nullptr, &out.image) != VK_SUCCESS) return false;
        VkMemoryRequirements r;
        vkGetImageMemoryRequirements(dev.device, out.image, &r);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = r.size;
        ai.memoryTypeIndex = memType(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (ai.memoryTypeIndex == ~0u) ai.memoryTypeIndex = memType(r.memoryTypeBits, 0);
        if (ai.memoryTypeIndex == ~0u || vkAllocateMemory(dev.device, &ai, nullptr, &out.memory) != VK_SUCCESS) return false;
        return vkBindImageMemory(dev.device, out.image, out.memory, 0) == VK_SUCCESS;
    }

    void freeImage(Image& im)
    {
        if (im.image) vkDestroyImage(dev.device, im.image, nullptr);
        if (im.memory) vkFreeMemory(dev.device, im.memory, nullptr);
        im = Image{};
    }

    VkImageView makeView(VkImage image, VkFormat fmt, VkImageAspectFlags aspect, uint32_t layer, VkImageUsageFlags usage)
    {
        VkImageViewUsageCreateInfo u{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
        u.usage = usage;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.pNext = usage ? &u : nullptr;
        vi.image = image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt;
        vi.subresourceRange = {aspect, 0, 1, layer, 1};
        VkImageView v = VK_NULL_HANDLE;
        vkCreateImageView(dev.device, &vi, nullptr, &v);
        return v;
    }

    void releaseConfig()
    {
        VkDevice d = dev.device;
        for (uint32_t s = 0; s < kSlots; ++s)
        {
            for (VkImageView* v : {&capView[s], &yView[s], &uvView[s], &srcView[s]})
                if (*v) { vkDestroyImageView(d, *v, nullptr); *v = VK_NULL_HANDLE; }
            freeImage(cap[s]); freeImage(yImg[s]); freeImage(uvImg[s]); freeImage(src[s]);
            if (bitsMap[s]) { vkUnmapMemory(d, bitsMem[s]); bitsMap[s] = nullptr; }
            if (bits[s]) { vkDestroyBuffer(d, bits[s], nullptr); bits[s] = VK_NULL_HANDLE; }
            if (bitsMem[s]) { vkFreeMemory(d, bitsMem[s], nullptr); bitsMem[s] = VK_NULL_HANDLE; }
            slots[s] = Slot{};
        }
        for (VkImageView& v : dpbView) if (v) { vkDestroyImageView(d, v, nullptr); v = VK_NULL_HANDLE; }
        freeImage(dpb);
        if (params) { fn.destroyParams(d, params, nullptr); params = VK_NULL_HANDLE; }
        if (session) { fn.destroySession(d, session, nullptr); session = VK_NULL_HANDLE; }
        for (VkDeviceMemory m : sessionMem) vkFreeMemory(d, m, nullptr);
        sessionMem.clear();
        spsNal.clear(); ppsNal.clear();
        configured = false;
        recordedSlot = -1;
    }

    // Sizes for a finished image of w x h; false if the encoder can't take it.
    bool configure(uint32_t w, uint32_t h, VkFormat format, const Config& cfg, std::string& err);
    void recordEncode(uint32_t s, bool idr);
};

// ---------------------------------------------------------------------------------------------

const std::vector<const char*>& GMDXReplayEncoder::deviceExtensions()
{
    static const std::vector<const char*> exts = {
        VK_KHR_VIDEO_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
        VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
    };
    return exts;
}

bool GMDXReplayEncoder::deviceSupport(VkPhysicalDevice physical, uint32_t& encodeFamily)
{
    encodeFamily = ~0u;
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> props(n);
    if (n) vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, props.data());
    for (const char* want : deviceExtensions())
    {
        bool found = false;
        for (const auto& p : props) found = found || std::strcmp(p.extensionName, want) == 0;
        if (!found) return false;
    }
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties2(physical, &qn, nullptr);
    std::vector<VkQueueFamilyVideoPropertiesKHR> video(qn, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR});
    std::vector<VkQueueFamilyProperties2> q(qn, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
    for (uint32_t i = 0; i < qn; ++i) q[i].pNext = &video[i];
    vkGetPhysicalDeviceQueueFamilyProperties2(physical, &qn, q.data());
    for (uint32_t i = 0; i < qn; ++i)
    {
        if ((q[i].queueFamilyProperties.queueFlags & VK_QUEUE_VIDEO_ENCODE_BIT_KHR)
            && (video[i].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR))
        {
            encodeFamily = i;
            return true;
        }
    }
    return false;
}

GMDXReplayEncoder::GMDXReplayEncoder() : d(new Impl) {}
GMDXReplayEncoder::~GMDXReplayEncoder()
{
    // no Vulkan calls here: the device may already be gone or lost (call shutdown() first)
    if (d && d->saver.joinable()) d->saver.join();
}

bool GMDXReplayEncoder::init(const Device& device, const uint32_t* spv, size_t spvBytes, std::string& err)
{
    shutdown();
    Impl& m = *d;
    m.dev = device;
    if (!device.device || !device.encodeQueue || !device.gfxQueue || !spv || !spvBytes) { err = "no encode queue"; return false; }
    VkDevice dv = device.device;
    vkGetPhysicalDeviceMemoryProperties(device.physical, &m.memProps);

#define GMDX_INST(f, name) m.fn.f = (decltype(m.fn.f))vkGetInstanceProcAddr(device.instance, name)
#define GMDX_DEV(f, name) m.fn.f = (decltype(m.fn.f))vkGetDeviceProcAddr(dv, name)
    GMDX_INST(getCaps, "vkGetPhysicalDeviceVideoCapabilitiesKHR");
    GMDX_INST(getFormats, "vkGetPhysicalDeviceVideoFormatPropertiesKHR");
    GMDX_DEV(createSession, "vkCreateVideoSessionKHR");
    GMDX_DEV(destroySession, "vkDestroyVideoSessionKHR");
    GMDX_DEV(sessionMemReqs, "vkGetVideoSessionMemoryRequirementsKHR");
    GMDX_DEV(bindSessionMem, "vkBindVideoSessionMemoryKHR");
    GMDX_DEV(createParams, "vkCreateVideoSessionParametersKHR");
    GMDX_DEV(destroyParams, "vkDestroyVideoSessionParametersKHR");
    GMDX_DEV(getEncodedParams, "vkGetEncodedVideoSessionParametersKHR");
    GMDX_DEV(beginCoding, "vkCmdBeginVideoCodingKHR");
    GMDX_DEV(endCoding, "vkCmdEndVideoCodingKHR");
    GMDX_DEV(controlCoding, "vkCmdControlVideoCodingKHR");
    GMDX_DEV(encode, "vkCmdEncodeVideoKHR");
#undef GMDX_INST
#undef GMDX_DEV
    g_barrier2 = (PFN_vkCmdPipelineBarrier2)vkGetDeviceProcAddr(dv, "vkCmdPipelineBarrier2");
    if (!g_barrier2) g_barrier2 = (PFN_vkCmdPipelineBarrier2)vkGetDeviceProcAddr(dv, "vkCmdPipelineBarrier2KHR");
    g_submit2 = (PFN_vkQueueSubmit2)vkGetDeviceProcAddr(dv, "vkQueueSubmit2");
    if (!g_submit2) g_submit2 = (PFN_vkQueueSubmit2)vkGetDeviceProcAddr(dv, "vkQueueSubmit2KHR");
    if (!g_barrier2 || !g_submit2) { err = "synchronization2 entry points missing"; return false; }
    const void* fns[] = {(void*)m.fn.getCaps, (void*)m.fn.getFormats, (void*)m.fn.createSession, (void*)m.fn.destroySession,
                         (void*)m.fn.sessionMemReqs, (void*)m.fn.bindSessionMem, (void*)m.fn.createParams,
                         (void*)m.fn.destroyParams, (void*)m.fn.getEncodedParams, (void*)m.fn.beginCoding,
                         (void*)m.fn.endCoding, (void*)m.fn.controlCoding, (void*)m.fn.encode};
    for (const void* p : fns)
        if (!p) { err = "video encode entry points missing (extensions not enabled?)"; return false; }

    // profile: H.264 Main, 4:2:0 8-bit
    m.h264Profile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
    m.profile.pNext = &m.h264Profile;
    m.profile.videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
    m.profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
    m.profile.lumaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    m.profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
    m.profileList.profileCount = 1;
    m.profileList.pProfiles = &m.profile;

    m.caps.pNext = &m.encCaps;
    m.encCaps.pNext = &m.h264Caps;
    if (m.fn.getCaps(device.physical, &m.profile, &m.caps) != VK_SUCCESS) { err = "H.264 Main encode not supported"; return false; }
    if (m.caps.maxDpbSlots < kDpbSlots || m.caps.maxActiveReferencePictures < 1 || m.h264Caps.maxPPictureL0ReferenceCount < 1)
    {
        err = "encoder can't do P frames with one reference";
        return false;
    }
    if (m.encCaps.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR)
        m.rcMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR;
    else if (m.encCaps.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR)
        m.rcMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR;
    if (!(m.encCaps.encodeInputPictureGranularity.width && m.encCaps.encodeInputPictureGranularity.height))
        m.encCaps.encodeInputPictureGranularity = {1, 1};

    auto pickFormat = [&](VkImageUsageFlags usage, VkFormat& out) {
        VkPhysicalDeviceVideoFormatInfoKHR fi{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR};
        fi.pNext = &m.profileList;
        fi.imageUsage = usage;
        uint32_t n = 0;
        if (m.fn.getFormats(device.physical, &fi, &n, nullptr) != VK_SUCCESS || !n) return false;
        std::vector<VkVideoFormatPropertiesKHR> f(n, {VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR});
        if (m.fn.getFormats(device.physical, &fi, &n, f.data()) != VK_SUCCESS) return false;
        for (const auto& p : f)
            if (p.format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM && p.imageTiling == VK_IMAGE_TILING_OPTIMAL) { out = p.format; return true; }
        return false;
    };
    if (!pickFormat(VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT, m.srcFormat)
        || !pickFormat(VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR, m.dpbFormat))
    {
        err = "no NV12 encode input / reference format";
        return false;
    }

    // conversion pipeline
    {
        VkDescriptorSetLayoutBinding b[3]{};
        b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 3; li.pBindings = b;
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1; pl.pSetLayouts = &m.dsl; pl.pushConstantRangeCount = 1; pl.pPushConstantRanges = &pr;
        VkShaderModuleCreateInfo sm{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        sm.codeSize = spvBytes; sm.pCode = spv;
        VkShaderModule mod = VK_NULL_HANDLE;
        if (vkCreateDescriptorSetLayout(dv, &li, nullptr, &m.dsl) != VK_SUCCESS
            || vkCreatePipelineLayout(dv, &pl, nullptr, &m.layout) != VK_SUCCESS
            || vkCreateShaderModule(dv, &sm, nullptr, &mod) != VK_SUCCESS)
        {
            err = "conversion pipeline setup failed";
            return false;
        }
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
        ci.layout = m.layout;
        const VkResult pr2 = vkCreateComputePipelines(dv, VK_NULL_HANDLE, 1, &ci, nullptr, &m.pipeline);
        vkDestroyShaderModule(dv, mod, nullptr);
        if (pr2 != VK_SUCCESS) { err = "conversion pipeline failed"; return false; }

        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kSlots}, {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2 * kSlots}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = kSlots; pi.poolSizeCount = 2; pi.pPoolSizes = ps;
        VkDescriptorSetLayout layouts[kSlots];
        for (auto& l : layouts) l = m.dsl;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorSetCount = kSlots; ai.pSetLayouts = layouts;
        if (vkCreateSampler(dv, &si, nullptr, &m.sampler) != VK_SUCCESS
            || vkCreateDescriptorPool(dv, &pi, nullptr, &m.pool) != VK_SUCCESS
            || (ai.descriptorPool = m.pool, vkAllocateDescriptorSets(dv, &ai, m.sets) != VK_SUCCESS))
        {
            err = "conversion descriptors failed";
            return false;
        }
    }

    // timelines, encode command buffers, feedback queries
    {
        VkSemaphoreTypeCreateInfo tc{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        tc.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo sc{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        sc.pNext = &tc;
        VkCommandPoolCreateInfo pc{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pc.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pc.queueFamilyIndex = device.encodeFamily;
        VkCommandBufferAllocateInfo ca{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ca.commandBufferCount = kSlots;
        VkQueryPoolVideoEncodeFeedbackCreateInfoKHR fb{VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR};
        fb.pNext = &m.profile;
        fb.encodeFeedbackFlags = VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR | VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.pNext = &fb; qi.queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR; qi.queryCount = kSlots;
        if (vkCreateSemaphore(dv, &sc, nullptr, &m.gfxTl) != VK_SUCCESS || vkCreateSemaphore(dv, &sc, nullptr, &m.encTl) != VK_SUCCESS
            || vkCreateCommandPool(dv, &pc, nullptr, &m.encPool) != VK_SUCCESS
            || (ca.commandPool = m.encPool, vkAllocateCommandBuffers(dv, &ca, m.encCmd) != VK_SUCCESS)
            || vkCreateQueryPool(dv, &qi, nullptr, &m.queries) != VK_SUCCESS)
        {
            err = "encode queue setup failed";
            return false;
        }
    }
    mReady = true;
    return true;
}

bool GMDXReplayEncoder::Impl::configure(uint32_t w, uint32_t h, VkFormat format, const Config& cfg, std::string& err)
{
    releaseConfig();
    VkDevice dv = dev.device;
    srcW = w; srcH = h;
    maxH = cfg.maxHeight;
    capFormat = unormTwin(format);
    fps = std::clamp(cfg.fps, 10u, 240u);
    gop = fps;
    bitrateKbps = std::clamp(cfg.bitrateKbps, 1000u, 200000u);

    // picture: scaled to maxHeight (aspect kept), even, within the encoder's extent
    double scale = std::min(1.0, (double)std::max(cfg.maxHeight, 144u) / h);
    scale = std::min(scale, (double)caps.maxCodedExtent.width / w);
    scale = std::min(scale, (double)caps.maxCodedExtent.height / h);
    picW = std::max(2u, (uint32_t)(w * scale) & ~1u);
    picH = std::max(2u, (uint32_t)(h * scale) & ~1u);
    const VkExtent2D g = caps.pictureAccessGranularity, ig = encCaps.encodeInputPictureGranularity;
    codedW = alignUp(alignUp(alignUp(std::max(picW, caps.minCodedExtent.width), 16), std::max(1u, g.width)), ig.width);
    codedH = alignUp(alignUp(alignUp(std::max(picH, caps.minCodedExtent.height), 16), std::max(1u, g.height)), ig.height);
    if (codedW > caps.maxCodedExtent.width || codedH > caps.maxCodedExtent.height || codedW % 16 || codedH % 16)
    {
        err = "picture size not encodable";
        return false;
    }

    const uint32_t qfi[2] = {dev.gfxFamily, dev.encodeFamily};
    for (uint32_t s = 0; s < kSlots; ++s)
    {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.mipLevels = 1; ci.arrayLayers = 1; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        ci.format = capFormat; ci.extent = {w, h, 1};
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        if (!makeImage(cap[s], ci, false)) { err = "capture image"; return false; }
        ci.format = VK_FORMAT_R8_UNORM; ci.extent = {codedW, codedH, 1};
        ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        if (!makeImage(yImg[s], ci, false)) { err = "luma image"; return false; }
        ci.format = VK_FORMAT_R8G8_UNORM; ci.extent = {codedW / 2, codedH / 2, 1};
        if (!makeImage(uvImg[s], ci, false)) { err = "chroma image"; return false; }
        ci.format = srcFormat; ci.extent = {codedW, codedH, 1};
        ci.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        ci.sharingMode = dev.gfxFamily != dev.encodeFamily ? VK_SHARING_MODE_CONCURRENT : VK_SHARING_MODE_EXCLUSIVE;
        ci.queueFamilyIndexCount = ci.sharingMode == VK_SHARING_MODE_CONCURRENT ? 2 : 0;
        ci.pQueueFamilyIndices = qfi;
        if (!makeImage(src[s], ci, true)) { err = "encode input image"; return false; }

        capView[s] = makeView(cap[s].image, capFormat, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0);
        yView[s] = makeView(yImg[s].image, VK_FORMAT_R8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0);
        uvView[s] = makeView(uvImg[s].image, VK_FORMAT_R8G8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT, 0, 0);
        srcView[s] = makeView(src[s].image, srcFormat, VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR);
        if (!capView[s] || !yView[s] || !uvView[s] || !srcView[s]) { err = "image views"; return false; }

        VkDescriptorImageInfo ii[3] = {{sampler, capView[s], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                                       {VK_NULL_HANDLE, yView[s], VK_IMAGE_LAYOUT_GENERAL},
                                       {VK_NULL_HANDLE, uvView[s], VK_IMAGE_LAYOUT_GENERAL}};
        VkWriteDescriptorSet wr[3]{};
        for (uint32_t k = 0; k < 3; ++k)
        {
            wr[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            wr[k].dstSet = sets[s]; wr[k].dstBinding = k; wr[k].descriptorCount = 1;
            wr[k].descriptorType = k == 0 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            wr[k].pImageInfo = &ii[k];
        }
        vkUpdateDescriptorSets(dv, 3, wr, 0, nullptr);

        // bitstream buffer, host visible
        bitsSize = std::max<VkDeviceSize>(kMinBitstream, (VkDeviceSize)codedW * codedH);
        const VkDeviceSize al = std::max<VkDeviceSize>(1, caps.minBitstreamBufferSizeAlignment);
        bitsSize = (bitsSize + al - 1) / al * al;
        VkVideoProfileListInfoKHR pl = profileList;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.pNext = &pl; bi.size = bitsSize; bi.usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR;
        if (vkCreateBuffer(dv, &bi, nullptr, &bits[s]) != VK_SUCCESS) { err = "bitstream buffer"; return false; }
        VkMemoryRequirements r;
        vkGetBufferMemoryRequirements(dv, bits[s], &r);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = r.size;
        ai.memoryTypeIndex = memType(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (ai.memoryTypeIndex == ~0u || vkAllocateMemory(dv, &ai, nullptr, &bitsMem[s]) != VK_SUCCESS
            || vkBindBufferMemory(dv, bits[s], bitsMem[s], 0) != VK_SUCCESS
            || vkMapMemory(dv, bitsMem[s], 0, VK_WHOLE_SIZE, 0, (void**)&bitsMap[s]) != VK_SUCCESS)
        {
            err = "bitstream memory";
            return false;
        }
    }

    // DPB: one image, a layer per slot
    {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D; ci.format = dpbFormat; ci.extent = {codedW, codedH, 1};
        ci.mipLevels = 1; ci.arrayLayers = kDpbSlots; ci.samples = VK_SAMPLE_COUNT_1_BIT; ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR; ci.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (!makeImage(dpb, ci, true)) { err = "reference image"; return false; }
        for (uint32_t i = 0; i < kDpbSlots; ++i)
            if (!(dpbView[i] = makeView(dpb.image, dpbFormat, VK_IMAGE_ASPECT_COLOR_BIT, i, 0))) { err = "reference views"; return false; }
    }

    // session + memory
    {
        VkVideoSessionCreateInfoKHR si{VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
        si.queueFamilyIndex = dev.encodeFamily;
        si.pVideoProfile = &profile;
        si.pictureFormat = srcFormat;
        si.maxCodedExtent = {codedW, codedH};
        si.referencePictureFormat = dpbFormat;
        si.maxDpbSlots = kDpbSlots;
        si.maxActiveReferencePictures = 1;
        si.pStdHeaderVersion = &caps.stdHeaderVersion;
        if (fn.createSession(dv, &si, nullptr, &session) != VK_SUCCESS) { err = "video session"; return false; }
        uint32_t n = 0;
        fn.sessionMemReqs(dv, session, &n, nullptr);
        std::vector<VkVideoSessionMemoryRequirementsKHR> reqs(n, {VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR});
        fn.sessionMemReqs(dv, session, &n, reqs.data());
        std::vector<VkBindVideoSessionMemoryInfoKHR> binds;
        for (const auto& r : reqs)
        {
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = r.memoryRequirements.size;
            ai.memoryTypeIndex = memType(r.memoryRequirements.memoryTypeBits, 0, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            VkDeviceMemory mem = VK_NULL_HANDLE;
            if (ai.memoryTypeIndex == ~0u || vkAllocateMemory(dv, &ai, nullptr, &mem) != VK_SUCCESS) { err = "session memory"; return false; }
            sessionMem.push_back(mem);
            VkBindVideoSessionMemoryInfoKHR b{VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR};
            b.memoryBindIndex = r.memoryBindIndex; b.memory = mem; b.memoryOffset = 0; b.memorySize = ai.allocationSize;
            binds.push_back(b);
        }
        if (!binds.empty() && fn.bindSessionMem(dv, session, (uint32_t)binds.size(), binds.data()) != VK_SUCCESS)
        {
            err = "session memory bind";
            return false;
        }
    }

    // parameters: SPS / PPS (the driver may adjust them; the bytes are read back)
    {
        StdVideoH264SequenceParameterSetVui vui{};
        vui.flags.video_signal_type_present_flag = 1;
        vui.flags.color_description_present_flag = 1;
        vui.video_format = 5;   // unspecified
        vui.colour_primaries = 1; vui.transfer_characteristics = 1; vui.matrix_coefficients = 1;   // BT.709
        StdVideoH264SequenceParameterSet sps{};
        sps.flags.frame_mbs_only_flag = 1;
        sps.flags.direct_8x8_inference_flag = 1;
        sps.flags.vui_parameters_present_flag = 1;
        sps.flags.frame_cropping_flag = (codedW != picW || codedH != picH) ? 1 : 0;
        sps.profile_idc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
        const uint32_t mbs = (codedW / 16) * (codedH / 16);
        StdVideoH264LevelIdc level = STD_VIDEO_H264_LEVEL_IDC_5_2;
        if (mbs <= 8704 && mbs * fps <= 522240) level = STD_VIDEO_H264_LEVEL_IDC_4_2;
        else if (mbs <= 36864 && mbs * fps <= 983040) level = STD_VIDEO_H264_LEVEL_IDC_5_1;
        sps.level_idc = std::min(level, h264Caps.maxLevelIdc);
        sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
        sps.log2_max_frame_num_minus4 = 4;   // frame_num wraps at 256 (a GOP is shorter)
        sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_2;
        sps.max_num_ref_frames = 1;
        sps.pic_width_in_mbs_minus1 = codedW / 16 - 1;
        sps.pic_height_in_map_units_minus1 = codedH / 16 - 1;
        sps.frame_crop_right_offset = (codedW - picW) / 2;
        sps.frame_crop_bottom_offset = (codedH - picH) / 2;
        sps.pSequenceParameterSetVui = &vui;
        StdVideoH264PictureParameterSet pps{};
        pps.flags.deblocking_filter_control_present_flag = 1;
        pps.flags.entropy_coding_mode_flag = (h264Caps.stdSyntaxFlags & VK_VIDEO_ENCODE_H264_STD_ENTROPY_CODING_MODE_FLAG_SET_BIT_KHR) ? 1 : 0;
        pps.weighted_bipred_idc = STD_VIDEO_H264_WEIGHTED_BIPRED_IDC_DEFAULT;

        VkVideoEncodeH264SessionParametersAddInfoKHR add{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR};
        add.stdSPSCount = 1; add.pStdSPSs = &sps; add.stdPPSCount = 1; add.pStdPPSs = &pps;
        VkVideoEncodeH264SessionParametersCreateInfoKHR hci{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR};
        hci.maxStdSPSCount = 1; hci.maxStdPPSCount = 1; hci.pParametersAddInfo = &add;
        VkVideoSessionParametersCreateInfoKHR pci{VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
        pci.pNext = &hci; pci.videoSession = session;
        if (fn.createParams(dv, &pci, nullptr, &params) != VK_SUCCESS) { err = "session parameters"; return false; }

        VkVideoEncodeH264SessionParametersGetInfoKHR hg{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR};
        hg.writeStdSPS = VK_TRUE; hg.writeStdPPS = VK_TRUE;
        VkVideoEncodeSessionParametersGetInfoKHR gi{VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR};
        gi.pNext = &hg; gi.videoSessionParameters = params;
        size_t n = 0;
        if (fn.getEncodedParams(dv, &gi, nullptr, &n, nullptr) != VK_SUCCESS || !n) { err = "SPS/PPS size"; return false; }
        std::vector<uint8_t> data(n);
        if (fn.getEncodedParams(dv, &gi, nullptr, &n, data.data()) != VK_SUCCESS) { err = "SPS/PPS"; return false; }
        data.resize(n);
        for (const auto& nal : splitNals(data))
        {
            const uint8_t type = nal.first[0] & 0x1F;
            if (type == 7) spsNal.assign(nal.first, nal.first + nal.second);
            if (type == 8) ppsNal.assign(nal.first, nal.first + nal.second);
        }
        if (spsNal.size() < 4 || ppsNal.empty()) { err = "SPS/PPS missing in the encoder's parameters"; return false; }
    }

    sessionReset = false;
    dpbReady = false;
    frameInGop = 0;
    encodes = 0;
    ring.clear();   // a new stream: older frames can't join it
    lastTick = INT64_MIN;
    configured = true;
    return true;
}

void GMDXReplayEncoder::Impl::recordEncode(uint32_t s, bool idr)
{
    VkCommandBuffer c = encCmd[s];
    vkResetCommandBuffer(c, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(c, &bi);
    vkCmdResetQueryPool(c, queries, s, 1);
    if (!dpbReady)
    {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        b.dstStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
        b.dstAccessMask = VK_ACCESS_2_VIDEO_ENCODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = dpb.image; b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kDpbSlots};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
        fnBarrier2(c, &di);
        dpbReady = true;
    }

    const uint32_t cur = encodes % kDpbSlots, prev = (encodes + 1) % kDpbSlots;
    const uint32_t frameNum = frameInGop % 256;

    VkVideoPictureResourceInfoKHR dpbRes[kDpbSlots];
    for (uint32_t i = 0; i < kDpbSlots; ++i)
    {
        dpbRes[i] = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
        dpbRes[i].codedExtent = {codedW, codedH};
        dpbRes[i].imageViewBinding = dpbView[i];
    }
    // reference info of this picture (the setup slot) and of the previous one
    StdVideoEncodeH264ReferenceInfo curRef{};
    curRef.primary_pic_type = idr ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
    curRef.FrameNum = frameNum;
    curRef.PicOrderCnt = (int32_t)(2 * frameNum);
    StdVideoEncodeH264ReferenceInfo prevRef{};
    const uint32_t prevNum = (frameInGop + 255) % 256;
    prevRef.primary_pic_type = frameInGop == 1 ? STD_VIDEO_H264_PICTURE_TYPE_IDR : STD_VIDEO_H264_PICTURE_TYPE_P;
    prevRef.FrameNum = prevNum;
    prevRef.PicOrderCnt = (int32_t)(2 * prevNum);
    VkVideoEncodeH264DpbSlotInfoKHR curDpb{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR};
    curDpb.pStdReferenceInfo = &curRef;
    VkVideoEncodeH264DpbSlotInfoKHR prevDpb{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR};
    prevDpb.pStdReferenceInfo = &prevRef;

    VkVideoReferenceSlotInfoKHR beginSlots[2];
    beginSlots[0] = {VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
    beginSlots[0].slotIndex = -1;   // the reconstructed picture (activated by the encode)
    beginSlots[0].pPictureResource = &dpbRes[cur];
    beginSlots[1] = {VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
    beginSlots[1].pNext = &prevDpb;
    beginSlots[1].slotIndex = (int32_t)prev;
    beginSlots[1].pPictureResource = &dpbRes[prev];

    // rate control (the session's state once set: every later scope must name it)
    VkVideoEncodeRateControlLayerInfoKHR layer{VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_LAYER_INFO_KHR};
    layer.averageBitrate = (uint64_t)bitrateKbps * 1000;
    layer.maxBitrate = rcMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR ? layer.averageBitrate * 3 / 2 : layer.averageBitrate;
    if (encCaps.maxBitrate)
    {
        layer.maxBitrate = std::min(layer.maxBitrate, encCaps.maxBitrate);
        layer.averageBitrate = std::min(layer.averageBitrate, layer.maxBitrate);
    }
    layer.frameRateNumerator = fps;
    layer.frameRateDenominator = 1;
    VkVideoEncodeH264RateControlInfoKHR h264rc{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_RATE_CONTROL_INFO_KHR};
    h264rc.gopFrameCount = gop;
    h264rc.idrPeriod = gop;
    h264rc.consecutiveBFrameCount = 0;
    h264rc.temporalLayerCount = 1;
    VkVideoEncodeRateControlInfoKHR rc{VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR};
    rc.pNext = &h264rc;
    rc.rateControlMode = rcMode;
    const bool rcLayers = rcMode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DEFAULT_KHR
                       && rcMode != VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
    rc.layerCount = rcLayers ? 1 : 0;
    rc.pLayers = rcLayers ? &layer : nullptr;
    rc.virtualBufferSizeInMs = rcLayers ? 1000 : 0;
    rc.initialVirtualBufferSizeInMs = rcLayers ? 500 : 0;

    VkVideoBeginCodingInfoKHR begin{VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
    begin.pNext = sessionReset ? &rc : nullptr;
    begin.videoSession = session;
    begin.videoSessionParameters = params;
    begin.referenceSlotCount = idr ? 1 : 2;
    begin.pReferenceSlots = beginSlots;
    fn.beginCoding(c, &begin);
    if (!sessionReset)
    {
        VkVideoCodingControlInfoKHR ctl{VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
        ctl.pNext = &rc;
        ctl.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR | VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR;
        fn.controlCoding(c, &ctl);
        sessionReset = true;
    }

    // picture
    StdVideoEncodeH264ReferenceListsInfo lists{};
    std::memset(lists.RefPicList0, STD_VIDEO_H264_NO_REFERENCE_PICTURE, sizeof(lists.RefPicList0));
    std::memset(lists.RefPicList1, STD_VIDEO_H264_NO_REFERENCE_PICTURE, sizeof(lists.RefPicList1));
    if (!idr) lists.RefPicList0[0] = (uint8_t)prev;
    StdVideoEncodeH264PictureInfo pic{};
    pic.flags.IdrPicFlag = idr ? 1 : 0;
    pic.flags.is_reference = 1;
    pic.idr_pic_id = (uint16_t)(idrId & 0xFFFF);
    pic.primary_pic_type = curRef.primary_pic_type;
    pic.frame_num = frameNum;
    pic.PicOrderCnt = curRef.PicOrderCnt;
    pic.pRefLists = &lists;
    StdVideoEncodeH264SliceHeader slice{};
    slice.slice_type = idr ? STD_VIDEO_H264_SLICE_TYPE_I : STD_VIDEO_H264_SLICE_TYPE_P;
    slice.cabac_init_idc = STD_VIDEO_H264_CABAC_INIT_IDC_0;
    slice.disable_deblocking_filter_idc = STD_VIDEO_H264_DISABLE_DEBLOCKING_FILTER_IDC_DISABLED;   // (0: the filter runs)
    VkVideoEncodeH264NaluSliceInfoKHR nalu{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR};
    nalu.pStdSliceHeader = &slice;
    VkVideoEncodeH264PictureInfoKHR hpic{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR};
    hpic.naluSliceEntryCount = 1;
    hpic.pNaluSliceEntries = &nalu;
    hpic.pStdPictureInfo = &pic;

    VkVideoReferenceSlotInfoKHR setup{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
    setup.pNext = &curDpb;
    setup.slotIndex = (int32_t)cur;
    setup.pPictureResource = &dpbRes[cur];
    VkVideoReferenceSlotInfoKHR ref{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
    ref.pNext = &prevDpb;
    ref.slotIndex = (int32_t)prev;
    ref.pPictureResource = &dpbRes[prev];

    VkVideoEncodeInfoKHR ei{VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR};
    ei.pNext = &hpic;
    ei.dstBuffer = bits[s];
    ei.dstBufferOffset = 0;
    ei.dstBufferRange = bitsSize;
    ei.srcPictureResource = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
    ei.srcPictureResource.codedExtent = {codedW, codedH};
    ei.srcPictureResource.imageViewBinding = srcView[s];
    ei.pSetupReferenceSlot = &setup;
    ei.referenceSlotCount = idr ? 0 : 1;
    ei.pReferenceSlots = idr ? nullptr : &ref;

    vkCmdBeginQuery(c, queries, s, 0);
    fn.encode(c, &ei);
    vkCmdEndQuery(c, queries, s);
    VkVideoEndCodingInfoKHR end{VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
    fn.endCoding(c, &end);
    // the bitstream -> host reads (poll)
    VkBufferMemoryBarrier2 hb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER_2};
    hb.srcStageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR; hb.srcAccessMask = VK_ACCESS_2_VIDEO_ENCODE_WRITE_BIT_KHR;
    hb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; hb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hb.buffer = bits[s]; hb.offset = 0; hb.size = VK_WHOLE_SIZE;
    VkDependencyInfo hd{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    hd.bufferMemoryBarrierCount = 1; hd.pBufferMemoryBarriers = &hb;
    fnBarrier2(c, &hd);
    vkEndCommandBuffer(c);

    ++encodes;
    frameInGop = (frameInGop + 1) % gop;
    if (idr) ++idrId;
}

// ---------------------------------------------------------------------------------------------

bool GMDXReplayEncoder::recordCapture(VkCommandBuffer cmd, VkImage srcImage, VkFormat srcFormat, uint32_t w, uint32_t h,
                                      double nowSec, const Config& cfg)
{
    if (!mReady || !cmd || !srcImage || w < 16 || h < 16) return false;
    Impl& m = *d;
    m.keepSeconds = std::clamp(cfg.seconds, 5u, 300u);
    // ponytail: 32-bit mdat -- seconds x bitrate capped near 3 GB; write a 64-bit mdat if longer clips are wanted
    const uint64_t kbps = std::clamp(cfg.bitrateKbps, 1000u, 200000u);
    m.keepSeconds = (uint32_t)std::min<uint64_t>(m.keepSeconds, 3000000000ull * 8 / (kbps * 1000));
    if (m.recordedSlot >= 0) return false;   // last capture not submitted yet

    // capture rate (a little slack so a 60 Hz display isn't halved by timer jitter)
    const double interval = 1.0 / std::clamp(cfg.fps, 10u, 240u);
    if (m.lastCapture >= 0.0 && nowSec - m.lastCapture < interval * 0.9) return false;

    const bool changed = !m.configured || w != m.srcW || h != m.srcH || unormTwin(srcFormat) != m.capFormat
                      || std::clamp(cfg.fps, 10u, 240u) != m.fps || std::clamp(cfg.bitrateKbps, 1000u, 200000u) != m.bitrateKbps
                      || cfg.maxHeight != m.maxH;
    if (changed)
    {
        // wait until the new size / settings have held for half a second (and past a failed try)
        if (w != m.wantW || h != m.wantH) { m.wantW = w; m.wantH = h; m.wantSince = nowSec; }
        if (nowSec - m.wantSince < 0.5 || nowSec < m.retryAt) return false;
        const VkResult wr = vkDeviceWaitIdle(m.dev.device);   // the slots' images may still be in use
        if (wr == VK_ERROR_DEVICE_LOST) { mReady = false; return false; }
        poll(nowSec);
        std::string err;
        if (!m.configure(w, h, srcFormat, cfg, err))
        {
            std::fprintf(stderr, "[gmdx replay] configure %ux%u failed: %s\n", w, h, err.c_str());
            m.releaseConfig();
            m.retryAt = nowSec + 10.0;
            m.error = "couldn't set up the encoder at " + std::to_string(w) + "x" + std::to_string(h) + " (" + err
                    + "); retrying every 10 s";
            return false;
        }
        std::fprintf(stderr, "[gmdx replay] encoding %ux%u (coded %ux%u) at %u fps, %u kbps, %s\n", m.picW, m.picH,
                     m.codedW, m.codedH, m.fps, m.bitrateKbps,
                     m.rcMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_VBR_BIT_KHR ? "VBR"
                     : m.rcMode == VK_VIDEO_ENCODE_RATE_CONTROL_MODE_CBR_BIT_KHR ? "CBR" : "default rate control");
    }

    poll(nowSec);
    int s = -1;
    for (uint32_t i = 0; i < kSlots && s < 0; ++i)
        if (m.slots[i].state == Impl::Free) s = (int)i;
    if (s < 0) { ++m.dropped; return false; }   // the encoder is behind: skip this frame

    // --- graphics: copy, convert, into the encode input ---
    auto barrier = [&](VkImage image, VkImageAspectFlags aspect, VkImageLayout from, VkImageLayout to,
                       VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                       VkAccessFlags2 dstAccess, uint32_t layers = 1) {
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = srcStage; b.srcAccessMask = srcAccess; b.dstStageMask = dstStage; b.dstAccessMask = dstAccess;
        b.oldLayout = from; b.newLayout = to;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image; b.subresourceRange = {aspect, 0, 1, 0, layers};
        VkDependencyInfo di{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        di.imageMemoryBarrierCount = 1; di.pImageMemoryBarriers = &b;
        fnBarrier2(cmd, &di);
    };
    constexpr VkPipelineStageFlags2 kCopy = VK_PIPELINE_STAGE_2_COPY_BIT;
    constexpr VkPipelineStageFlags2 kCompute = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    constexpr VkPipelineStageFlags2 kColor = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    barrier(srcImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            kColor, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, kCopy, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(m.cap[s].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            kCompute, 0, kCopy, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy cp{};
    cp.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cp.extent = {w, h, 1};
    vkCmdCopyImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m.cap[s].image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    // the finished image goes back for the caller's present transition
    barrier(srcImage, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            kCopy, 0, kColor, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);

    barrier(m.cap[s].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            kCopy, VK_ACCESS_2_TRANSFER_WRITE_BIT, kCompute, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    barrier(m.yImg[s].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            kCopy, 0, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    barrier(m.uvImg[s].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
            kCopy, 0, kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.pipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m.layout, 0, 1, &m.sets[s], 0, nullptr);
    const int32_t push[4] = {(int32_t)m.picW, (int32_t)m.picH, (int32_t)m.codedW, (int32_t)m.codedH};
    vkCmdPushConstants(cmd, m.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
    vkCmdDispatch(cmd, (m.codedW / 2 + 7) / 8, (m.codedH / 2 + 7) / 8, 1);

    barrier(m.yImg[s].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCopy, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(m.uvImg[s].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            kCompute, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, kCopy, VK_ACCESS_2_TRANSFER_READ_BIT);
    barrier(m.src[s].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            kCopy, 0, kCopy, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy planes[2]{};
    planes[0].srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    planes[0].dstSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
    planes[0].extent = {m.codedW, m.codedH, 1};
    vkCmdCopyImage(cmd, m.yImg[s].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m.src[s].image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &planes[0]);
    planes[1].srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    planes[1].dstSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
    planes[1].extent = {m.codedW / 2, m.codedH / 2, 1};
    vkCmdCopyImage(cmd, m.uvImg[s].image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m.src[s].image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &planes[1]);
    // -> the encoder's input layout; the encode queue waits on mGfxTl (submit())
    barrier(m.src[s].image, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR,
            kCopy, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);

    m.slots[s].state = Impl::Recorded;
    m.slots[s].t = nowSec;
    m.recordedSlot = s;
    m.lastCapture = (m.lastCapture >= 0.0 && nowSec - m.lastCapture < interval * 1.5) ? m.lastCapture + interval : nowSec;
    return true;
}

void GMDXReplayEncoder::submit()
{
    if (!mReady) return;
    Impl& m = *d;
    if (m.recordedSlot < 0) return;
    const uint32_t s = (uint32_t)m.recordedSlot;
    m.recordedSlot = -1;
    const uint64_t v = ++m.nextValue;

    // graphics: signal after everything submitted so far (the frame that recorded the capture)
    VkSemaphoreSubmitInfo gs{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    gs.semaphore = m.gfxTl; gs.value = v; gs.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 g{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    g.signalSemaphoreInfoCount = 1; g.pSignalSemaphoreInfos = &gs;
    if (fnSubmit2(m.dev.gfxQueue, 1, &g, VK_NULL_HANDLE) != VK_SUCCESS)
    {
        m.slots[s].state = Impl::Free;
        mReady = false;
        return;
    }

    // an IDR every gop frames, and at least once a second of capture time (at a low frame rate
    // a frame-count GOP spans many seconds, and a clip can only start on an IDR)
    const bool idr = m.frameInGop == 0 || m.slots[s].t - m.lastIdrT >= 1.0;
    if (idr)
    {
        m.frameInGop = 0;
        m.lastIdrT = m.slots[s].t;
    }
    m.recordEncode(s, idr);
    m.slots[s].idr = idr;
    VkSemaphoreSubmitInfo ws{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    ws.semaphore = m.gfxTl; ws.value = v; ws.stageMask = VK_PIPELINE_STAGE_2_VIDEO_ENCODE_BIT_KHR;
    VkSemaphoreSubmitInfo es{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    es.semaphore = m.encTl; es.value = v; es.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = m.encCmd[s];
    VkSubmitInfo2 e{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    e.waitSemaphoreInfoCount = 1; e.pWaitSemaphoreInfos = &ws;
    e.commandBufferInfoCount = 1; e.pCommandBufferInfos = &cb;
    e.signalSemaphoreInfoCount = 1; e.pSignalSemaphoreInfos = &es;
    if (fnSubmit2(m.dev.encodeQueue, 1, &e, VK_NULL_HANDLE) != VK_SUCCESS)
    {
        // the signal on the graphics queue still happens; nothing else waits on it
        m.slots[s].state = Impl::Free;
        mReady = false;
        return;
    }
    m.slots[s].state = Impl::Encoding;
    m.slots[s].value = v;
}

void GMDXReplayEncoder::poll(double nowSec)
{
    if (!mReady) return;
    Impl& m = *d;
    uint64_t done = 0;
    if (vkGetSemaphoreCounterValue(m.dev.device, m.encTl, &done) != VK_SUCCESS) return;
    // harvest in submission order (the stream must stay ordered)
    for (;;)
    {
        int next = -1;
        for (uint32_t i = 0; i < kSlots; ++i)
            if (m.slots[i].state == Impl::Encoding && m.slots[i].value <= done
                && (next < 0 || m.slots[i].value < m.slots[next].value))
                next = (int)i;
        if (next < 0) break;
        Impl::Slot& sl = m.slots[next];
        uint32_t res[3] = {};
        const VkResult qr = vkGetQueryPoolResults(m.dev.device, m.queries, (uint32_t)next, 1, sizeof(res), res, sizeof(res),
                                                  VK_QUERY_RESULT_WITH_STATUS_BIT_KHR);
        const int32_t status = (int32_t)res[2];
        const bool good = qr == VK_SUCCESS && status > 0 && res[1] > 0 && (VkDeviceSize)res[0] + res[1] <= m.bitsSize;
        if (good && sl.idr) m.skipUntilIdr = false;
        if (good && m.skipUntilIdr)
        {
            ++m.dropped;   // references the lost picture
        }
        else if (good)
        {
            auto p = std::make_shared<Packet>();
            p->annexB.assign(m.bitsMap[next] + res[0], m.bitsMap[next] + res[0] + res[1]);
            // on the 1/fps grid, never two on one tick: an even timeline for players and editors
            // (captures may bunch a little; the drift is bounded because the rate is capped at fps)
            const int64_t tick = std::max<int64_t>((int64_t)std::llround(sl.t * m.fps), m.lastTick + 1);
            m.lastTick = tick;
            p->t = (double)tick / m.fps;
            p->idr = sl.idr;
            m.ring.push_back(std::move(p));
            ++m.encoded;
        }
        else
        {
            // a lost picture breaks the reference chain: restart it
            ++m.dropped;
            m.frameInGop = 0;
            m.skipUntilIdr = true;
            std::fprintf(stderr, "[gmdx replay] encode failed (query %d, status %d, %u bytes)\n", (int)qr, status, res[1]);
        }
        sl.state = Impl::Free;
    }

    // keep keepSeconds (+ the GOP before them): drop up to the newest IDR older than the cutoff
    const double cutoff = nowSec - m.keepSeconds;
    size_t lastOldIdr = SIZE_MAX;
    for (size_t i = 0; i < m.ring.size() && m.ring[i]->t < cutoff; ++i)
        if (m.ring[i]->idr) lastOldIdr = i;
    if (lastOldIdr != SIZE_MAX && lastOldIdr > 0)
        m.ring.erase(m.ring.begin(), m.ring.begin() + (std::ptrdiff_t)lastOldIdr);
    while (!m.ring.empty() && !m.ring.front()->idr) m.ring.pop_front();   // a clip always starts on an IDR
}

bool GMDXReplayEncoder::saveAsync(const std::string& path, uint32_t seconds, double nowSec, GMDXReplayAudioClip audio)
{
    if (!mReady) return false;
    Impl& m = *d;
    if (m.saving.load()) return false;
    poll(nowSec);
    // from the newest IDR at or before now - seconds (else the oldest)
    const double from = nowSec - std::max(1u, seconds);
    size_t start = 0;
    for (size_t i = 0; i < m.ring.size() && m.ring[i]->t <= from; ++i)
        if (m.ring[i]->idr) start = i;
    std::vector<PacketPtr> clip(m.ring.begin() + (std::ptrdiff_t)start, m.ring.end());
    if (clip.empty()) return false;
    if (m.saver.joinable()) m.saver.join();
    m.saving = true;
    m.saver = std::thread([this, path, clip = std::move(clip), sps = m.spsNal, pps = m.ppsNal,
                           w = m.picW, h = m.picH, fps = m.fps, audio = std::move(audio)]() {
        std::string msg;
        const bool ok = writeMp4(path, sps, pps, w, h, fps, clip, audio, msg);
        std::lock_guard<std::mutex> lk(d->resultMutex);
        d->haveResult = true;
        d->resultOk = ok;
        d->resultMsg = ok ? path + " (" + msg + ")" : msg;
        d->saving = false;
    });
    return true;
}

bool GMDXReplayEncoder::takeSaveResult(bool& ok, std::string& message)
{
    std::lock_guard<std::mutex> lk(d->resultMutex);
    if (!d->haveResult) return false;
    d->haveResult = false;
    ok = d->resultOk;
    message = d->resultMsg;
    return true;
}

void GMDXReplayEncoder::cancel()
{
    Impl& m = *d;
    if (m.recordedSlot < 0) return;
    m.slots[m.recordedSlot].state = Impl::Free;
    m.recordedSlot = -1;
}

bool GMDXReplayEncoder::takeError(std::string& message)
{
    if (d->error.empty()) return false;
    message.swap(d->error);
    d->error.clear();
    return true;
}

void GMDXReplayEncoder::abandon()
{
    if (!d) return;
    if (d->saver.joinable()) d->saver.join();
    (void)d.release();   // ponytail: leaked on purpose -- vkDestroy* on a lost device can crash or hang drivers
    d.reset(new Impl);
    mReady = false;
}

bool GMDXReplayEncoder::saving() const
{
    return d->saving.load();
}

double GMDXReplayEncoder::bufferedSeconds() const
{
    const Impl& m = *d;
    return m.ring.size() < 2 ? 0.0 : m.ring.back()->t - m.ring.front()->t;
}

std::string GMDXReplayEncoder::describe() const
{
    const Impl& m = *d;
    char buf[200];
    std::snprintf(buf, sizeof(buf), "%s %ux%u, %.1f s buffered (%zu frames), %llu encoded, %llu dropped",
                  mReady ? "ready" : "off", m.picW, m.picH, bufferedSeconds(), m.ring.size(),
                  (unsigned long long)m.encoded, (unsigned long long)m.dropped);
    return buf;
}

void GMDXReplayEncoder::shutdown()
{
    if (!d) return;
    Impl& m = *d;
    if (m.saver.joinable()) m.saver.join();
    if (!m.dev.device) return;
    VkDevice dv = m.dev.device;
    vkDeviceWaitIdle(dv);
    m.releaseConfig();
    if (m.queries) vkDestroyQueryPool(dv, m.queries, nullptr);
    if (m.encPool) vkDestroyCommandPool(dv, m.encPool, nullptr);
    if (m.gfxTl) vkDestroySemaphore(dv, m.gfxTl, nullptr);
    if (m.encTl) vkDestroySemaphore(dv, m.encTl, nullptr);
    if (m.pool) vkDestroyDescriptorPool(dv, m.pool, nullptr);
    if (m.sampler) vkDestroySampler(dv, m.sampler, nullptr);
    if (m.pipeline) vkDestroyPipeline(dv, m.pipeline, nullptr);
    if (m.layout) vkDestroyPipelineLayout(dv, m.layout, nullptr);
    if (m.dsl) vkDestroyDescriptorSetLayout(dv, m.dsl, nullptr);
    d.reset(new Impl);
    mReady = false;
}
