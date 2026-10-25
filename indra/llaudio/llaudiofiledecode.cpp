/**
 * @file llaudiofiledecode.cpp
 * @brief Decode WAV / Ogg Vorbis / Opus / MP3 / FLAC files to mono PCM for the sound uploader.
 *
 * FLAC and MP3 use the vendored dr_libs single-header decoders (third_party/dr_libs,
 * public domain / MIT-0). Ogg Vorbis uses libvorbisfile. Opus uses libopusfile and is
 * compiled in only when LL_OPUSFILE is defined (see cmake/Audio.cmake).
 *
 * All input is untrusted: files are read into memory under a size cap, decoded in
 * streaming chunks, and the decoded frame count (not any header claim) is capped
 * before more memory is allocated.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_AUDIODECODE_STANDALONE
#include "linden_common.h"
#include "llfile.h"
#endif

#include "llaudiofiledecode.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <memory>

#define DR_FLAC_NO_STDIO
#define DR_FLAC_IMPLEMENTATION
#include <dr_flac.h>
#define DR_MP3_NO_STDIO
#define DR_MP3_IMPLEMENTATION
#include <dr_mp3.h>

#include <vorbis/vorbisfile.h>

#ifdef LL_OPUSFILE
#include <opusfile.h>
#endif

namespace
{

int fail(std::string& err, const char* label, int code)
{
    err = label;
    return code;
}

// Collects interleaved 16-bit PCM into a mono buffer (or, mKeepChannels, keeps it
// interleaved as it came), enforcing the decoded-size cap.
struct Sink
{
    std::vector<int16_t>& mOut;
    uint32_t mChannels = 0;
    uint64_t mMaxFrames = 0;
    uint64_t mFrames = 0;
    bool     mOverflow = false;
    bool     mKeepChannels = false;

    explicit Sink(std::vector<int16_t>& out, bool keep_channels = false) : mOut(out), mKeepChannels(keep_channels) {}

    void begin(uint32_t rate, uint32_t channels)
    {
        mChannels = channels;
        mMaxFrames = (uint64_t)(LLAUDIO_DECODE_MAX_SECONDS * (double)rate);
        mFrames = 0;
        mOut.clear();
    }

    // Returns false (and latches mOverflow) when the cap would be exceeded.
    bool append(const int16_t* interleaved, size_t frames)
    {
        if (mFrames + frames > mMaxFrames)
        {
            mOverflow = true;
            return false;
        }
        mFrames += frames;
        if (mKeepChannels || mChannels == 1)
        {
            const size_t n = frames * (mKeepChannels ? mChannels : 1);
            const size_t base = mOut.size();
            mOut.resize(base + n);
            std::memcpy(mOut.data() + base, interleaved, n * sizeof(int16_t));
            return true;
        }
        const size_t base = mOut.size();
        mOut.resize(base + frames);
        for (size_t i = 0; i < frames; ++i)
        {
            mOut[base + i] = (int16_t)(((int)interleaved[2 * i] + (int)interleaved[2 * i + 1]) / 2);
        }
        return true;
    }
};

uint32_t rd32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }
uint16_t rd16(const uint8_t* p) { return (uint16_t)(p[0] | (p[1] << 8)); }

// ---- WAV -------------------------------------------------------------------

int decode_wav(const uint8_t* d, size_t n, Sink& sink, uint32_t& rate, uint32_t& channels, std::string& err)
{
    if (n < 12 || std::memcmp(d, "RIFF", 4) || std::memcmp(d + 8, "WAVE", 4))
    {
        return fail(err, "SoundFileNotRIFF", LLAUDIO_DEC_WAV_FORMAT_ERR);
    }
    bool pcm = false;
    bool have_fmt = false;
    uint32_t bits = 0;
    size_t data_pos = 0, data_len = 0;
    bool have_data = false;

    size_t pos = 12;
    while (pos + 8 <= n)
    {
        const uint32_t len = rd32(d + pos + 4);
        const size_t body = pos + 8;
        if (len > n - body)
        {
            // Same rule as the legacy WAV check: a chunk may not run past the file.
            return fail(err, "SoundFileInvalidChunkSize", LLAUDIO_DEC_CHUNK_SIZE_ERR);
        }
        if (!std::memcmp(d + pos, "fmt ", 4))
        {
            if (len < 16)
            {
                return fail(err, "SoundFileInvalidHeader", LLAUDIO_DEC_WAV_FORMAT_ERR);
            }
            pcm = (rd16(d + body) == 1);
            channels = rd16(d + body + 2);
            rate = rd32(d + body + 4);
            bits = rd16(d + body + 14);
            have_fmt = true;
        }
        else if (!std::memcmp(d + pos, "data", 4) && !have_data)
        {
            data_pos = body;
            data_len = len;
            have_data = true;
        }
        pos = body + len + (len & 1);
    }

    if (!have_fmt || !pcm)
    {
        return fail(err, "SoundFileNotPCM", LLAUDIO_DEC_PCM_FORMAT_ERR);
    }
    if (channels < 1 || channels > 2)
    {
        return fail(err, "SoundFileInvalidChannelCount", LLAUDIO_DEC_CHANNEL_ERR);
    }
    if (rate != LLAUDIO_UPLOAD_SAMPLE_RATE)
    {
        // Existing uploader behaviour: WAV must already be 44.1 kHz.
        return fail(err, "SoundFileInvalidSampleRate", LLAUDIO_DEC_SAMPLE_RATE_ERR);
    }
    if (bits != 16 && bits != 8)
    {
        return fail(err, "SoundFileInvalidWordSize", LLAUDIO_DEC_WORD_SIZE_ERR);
    }
    if (!have_data || !data_len)
    {
        return fail(err, "SoundFileInvalidHeader", LLAUDIO_DEC_WAV_FORMAT_ERR);
    }

    sink.begin(rate, 1);   // frames are converted to mono here, so the sink sees 1 channel
    const size_t bytes_per_sample = bits / 8;
    const size_t frame_bytes = bytes_per_sample * channels;
    const size_t frames = data_len / frame_bytes;
    if ((uint64_t)frames > sink.mMaxFrames)
    {
        return fail(err, "SoundFileTooLongToSplit", LLAUDIO_DEC_TOO_LONG);
    }
    sink.mOut.resize(frames);
    const uint8_t* s = d + data_pos;
    for (size_t i = 0; i < frames; ++i)
    {
        int v[2] = {0, 0};
        for (uint32_t c = 0; c < channels; ++c)
        {
            if (bits == 16)
            {
                v[c] = (int16_t)rd16(s + (i * channels + c) * 2);
            }
            else
            {
                v[c] = ((int)s[i * channels + c] - 128) * 256;   // 8-bit WAV is unsigned
            }
        }
        sink.mOut[i] = (int16_t)(channels == 2 ? (v[0] + v[1]) / 2 : v[0]);
    }
    return LLAUDIO_DEC_OK;
}

// ---- Ogg Vorbis ------------------------------------------------------------

struct MemFile
{
    const uint8_t* mData;
    size_t         mSize;
    size_t         mPos;
};

size_t mem_read(void* ptr, size_t size, size_t nmemb, void* ds)
{
    MemFile* m = static_cast<MemFile*>(ds);
    if (!size || !nmemb) return 0;
    const size_t avail = m->mSize - m->mPos;
    const size_t want = (nmemb > avail / size) ? avail / size : nmemb;   // whole items, no overflow
    std::memcpy(ptr, m->mData + m->mPos, want * size);
    m->mPos += want * size;
    return want;
}

int mem_seek(void* ds, ogg_int64_t off, int whence)
{
    MemFile* m = static_cast<MemFile*>(ds);
    ogg_int64_t base = 0;
    switch (whence)
    {
    case SEEK_SET: base = 0; break;
    case SEEK_CUR: base = (ogg_int64_t)m->mPos; break;
    case SEEK_END: base = (ogg_int64_t)m->mSize; break;
    default: return -1;
    }
    const ogg_int64_t np = base + off;
    if (np < 0 || np > (ogg_int64_t)m->mSize) return -1;
    m->mPos = (size_t)np;
    return 0;
}

long mem_tell(void* ds)
{
    return (long)static_cast<MemFile*>(ds)->mPos;
}

int decode_vorbis(const uint8_t* d, size_t n, Sink& sink, uint32_t& rate, uint32_t& channels, std::string& err)
{
    MemFile mf{d, n, 0};
    ov_callbacks cb;
    cb.read_func = mem_read;
    cb.seek_func = mem_seek;
    cb.close_func = nullptr;
    cb.tell_func = mem_tell;

    OggVorbis_File vf;
    if (ov_open_callbacks(&mf, &vf, nullptr, 0, cb) != 0)
    {
        return fail(err, "SoundFileCorrupt", LLAUDIO_DEC_CORRUPT);
    }

    int rc = LLAUDIO_DEC_OK;
    do
    {
        vorbis_info* vi = ov_info(&vf, -1);
        if (!vi || vi->rate <= 0 || vi->channels <= 0)
        {
            rc = fail(err, "SoundFileCorrupt", LLAUDIO_DEC_CORRUPT);
            break;
        }
        rate = (uint32_t)vi->rate;
        channels = (uint32_t)vi->channels;
        if (channels > 2)
        {
            rc = fail(err, "SoundFileInvalidChannelCount", LLAUDIO_DEC_CHANNEL_ERR);
            break;
        }
        if (rate > LLAUDIO_DECODE_MAX_SOURCE_RATE)
        {
            rc = fail(err, "SoundFileInvalidSampleRate", LLAUDIO_DEC_SAMPLE_RATE_ERR);
            break;
        }
        sink.begin(rate, channels);

        int16_t buf[4096];
        int cur_link = -1;
        int holes = 0;
        uint64_t got = 0;
        for (;;)
        {
            int link = 0;
            const long r = ov_read(&vf, reinterpret_cast<char*>(buf), (int)sizeof(buf), 0, 2, 1, &link);
            if (r == 0) break;
            if (r < 0)
            {
                if (r == OV_HOLE && ++holes < 1000) continue;   // recoverable gap in the data
                rc = fail(err, "SoundFileCorrupt", LLAUDIO_DEC_CORRUPT);
                break;
            }
            if (link != cur_link)
            {
                vorbis_info* li = ov_info(&vf, link);
                if (!li || (uint32_t)li->rate != rate || (uint32_t)li->channels != channels)
                {
                    // Chained stream whose parameters change part way through.
                    rc = fail(err, "SoundFileUnsupportedCodec", LLAUDIO_DEC_UNSUPPORTED_CODEC);
                    break;
                }
                cur_link = link;
            }
            got += (uint64_t)r / (2 * channels);
            if (!sink.append(buf, (size_t)r / (2 * channels)))
            {
                rc = fail(err, "SoundFileTooLongToSplit", LLAUDIO_DEC_TOO_LONG);
                break;
            }
        }
        if (rc == LLAUDIO_DEC_OK)
        {
            // A damaged / truncated stream ends early: compare with the stream's own length.
            const ogg_int64_t total = ov_pcm_total(&vf, -1);
            if (total > 0 && got < (uint64_t)total)
            {
                rc = fail(err, "SoundFileCorrupt", LLAUDIO_DEC_CORRUPT);
            }
        }
    } while (false);

    ov_clear(&vf);
    return rc;
}

// ---- Opus (optional) ---------------------------------------------------------

#ifdef LL_OPUSFILE
int decode_opus(const uint8_t* d, size_t n, Sink& sink, uint32_t& rate, uint32_t& channels, std::string& err)
{
    int oerr = 0;
    OggOpusFile* of = op_open_memory(d, n, &oerr);
    if (!of)
    {
        return fail(err, "SoundFileCorrupt", LLAUDIO_DEC_CORRUPT);
    }
    int rc = LLAUDIO_DEC_OK;
    // libopusfile always decodes at 48 kHz; op_read_stereo folds any layout to stereo.
    rate = 48000;
    channels = (uint32_t)std::max(1, op_channel_count(of, -1));
    sink.begin(rate, 2);
    int16_t buf[2 * 2880];
    int holes = 0;
    for (;;)
    {
        const int r = op_read_stereo(of, buf, (int)(sizeof(buf) / sizeof(buf[0])));
        if (r == 0) break;
        if (r < 0)
        {
            if (r == OP_HOLE && ++holes < 1000) continue;
            rc = fail(err, "SoundFileCorrupt", LLAUDIO_DEC_CORRUPT);
            break;
        }
        if (!sink.append(buf, (size_t)r))
        {
            rc = fail(err, "SoundFileTooLongToSplit", LLAUDIO_DEC_TOO_LONG);
            break;
        }
    }
    op_free(of);
    return rc;
}
#endif

// ---- FLAC ------------------------------------------------------------------

int decode_flac(const uint8_t* d, size_t n, Sink& sink, uint32_t& rate, uint32_t& channels, std::string& err)
{
    drflac* fl = drflac_open_memory(d, n, nullptr);
    if (!fl)
    {
        return fail(err, "SoundFileCorrupt", LLAUDIO_DEC_CORRUPT);
    }
    int rc = LLAUDIO_DEC_OK;
    do
    {
        rate = fl->sampleRate;
        channels = fl->channels;
        if (channels < 1 || channels > 2)
        {
            rc = fail(err, "SoundFileInvalidChannelCount", LLAUDIO_DEC_CHANNEL_ERR);
            break;
        }
        if (rate == 0 || rate > LLAUDIO_DECODE_MAX_SOURCE_RATE)
        {
            rc = fail(err, "SoundFileInvalidSampleRate", LLAUDIO_DEC_SAMPLE_RATE_ERR);
            break;
        }
        sink.begin(rate, channels);
        const uint64_t claimed = fl->totalPCMFrameCount;   // 0 = unknown
        int16_t buf[4096];
        const drflac_uint64 per_read = sizeof(buf) / sizeof(buf[0]) / channels;
        uint64_t got = 0;
        for (;;)
        {
            const drflac_uint64 r = drflac_read_pcm_frames_s16(fl, per_read, buf);
            if (r == 0) break;
            got += r;
            if (!sink.append(buf, (size_t)r))
            {
                rc = fail(err, "SoundFileTooLongToSplit", LLAUDIO_DEC_TOO_LONG);
                break;
            }
        }
        if (rc == LLAUDIO_DEC_OK && claimed && got < claimed)
        {
            rc = fail(err, "SoundFileCorrupt", LLAUDIO_DEC_CORRUPT);   // truncated / damaged
        }
    } while (false);
    drflac_close(fl);
    return rc;
}

// ---- MP3 -------------------------------------------------------------------

int decode_mp3(const uint8_t* d, size_t n, Sink& sink, uint32_t& rate, uint32_t& channels, std::string& err)
{
    std::unique_ptr<drmp3> mp3(new drmp3);
    if (!drmp3_init_memory(mp3.get(), d, n, nullptr))
    {
        return fail(err, "SoundFileCorrupt", LLAUDIO_DEC_CORRUPT);
    }
    int rc = LLAUDIO_DEC_OK;
    do
    {
        rate = mp3->sampleRate;
        channels = mp3->channels;
        if (channels < 1 || channels > 2)
        {
            rc = fail(err, "SoundFileInvalidChannelCount", LLAUDIO_DEC_CHANNEL_ERR);
            break;
        }
        if (rate == 0 || rate > LLAUDIO_DECODE_MAX_SOURCE_RATE)
        {
            rc = fail(err, "SoundFileInvalidSampleRate", LLAUDIO_DEC_SAMPLE_RATE_ERR);
            break;
        }
        sink.begin(rate, channels);
        // drmp3 skips the LAME/Xing encoder delay and drops the trailing padding itself,
        // so decoded clips start at the first real sample (no leading silence).
        int16_t buf[4096];
        const drmp3_uint64 per_read = sizeof(buf) / sizeof(buf[0]) / channels;
        for (;;)
        {
            const drmp3_uint64 r = drmp3_read_pcm_frames_s16(mp3.get(), per_read, buf);
            if (r == 0) break;
            if (!sink.append(buf, (size_t)r))
            {
                rc = fail(err, "SoundFileTooLongToSplit", LLAUDIO_DEC_TOO_LONG);
                break;
            }
        }
    } while (false);
    drmp3_uninit(mp3.get());
    return rc;
}

} // namespace

// ---------------------------------------------------------------------------

bool llaudio_opus_available()
{
#ifdef LL_OPUSFILE
    return true;
#else
    return false;
#endif
}

LLAudioFormat llaudio_detect_format(const uint8_t* d, size_t n, const std::string& ext_lc)
{
    if (n >= 12 && !std::memcmp(d, "RIFF", 4) && !std::memcmp(d + 8, "WAVE", 4))
    {
        return LLAudioFormat::Wav;
    }
    if (n >= 4 && !std::memcmp(d, "fLaC", 4))
    {
        return LLAudioFormat::Flac;
    }
    if (n >= 4 && !std::memcmp(d, "OggS", 4))
    {
        // First page: 27-byte header, then a segment table, then the first packet.
        if (n >= 27)
        {
            const size_t nseg = d[26];
            const size_t pkt = 27 + nseg;
            if (n >= pkt + 8)
            {
                if (!std::memcmp(d + pkt, "\x01vorbis", 7)) return LLAudioFormat::OggVorbis;
                if (!std::memcmp(d + pkt, "OpusHead", 8)) return LLAudioFormat::Opus;
                if (!std::memcmp(d + pkt, "\x7f" "FLAC", 5)) return LLAudioFormat::Flac;
            }
        }
        return LLAudioFormat::Unknown;   // Ogg container with some other codec (Speex, Theora, ...)
    }
    if (n >= 3 && !std::memcmp(d, "ID3", 3))
    {
        return LLAudioFormat::Mp3;
    }
    if (n >= 2 && d[0] == 0xFF && (d[1] & 0xE0) == 0xE0)
    {
        return LLAudioFormat::Mp3;
    }
    if (ext_lc == "mp3")
    {
        return LLAudioFormat::Mp3;   // junk before the first frame; the decoder resyncs
    }
    return LLAudioFormat::Unknown;
}

int llaudio_decode_memory(const uint8_t* data, size_t size, const std::string& ext_lc,
                          LLAudioClip& clip, std::string& error_msg)
{
    error_msg.clear();
    clip = LLAudioClip();

    if (!data || size == 0)
    {
        return fail(error_msg, "SoundFileEmpty", LLAUDIO_DEC_EMPTY);
    }

    const LLAudioFormat fmt = llaudio_detect_format(data, size, ext_lc);
    std::vector<int16_t> mono;
    Sink sink(mono);
    uint32_t rate = 0, channels = 0;
    int rc = LLAUDIO_DEC_OK;

    switch (fmt)
    {
    case LLAudioFormat::Wav:
        clip.mFormatName = "WAV";
        rc = decode_wav(data, size, sink, rate, channels, error_msg);
        break;
    case LLAudioFormat::OggVorbis:
        clip.mFormatName = "Ogg Vorbis";
        rc = decode_vorbis(data, size, sink, rate, channels, error_msg);
        break;
    case LLAudioFormat::Opus:
        clip.mFormatName = "Opus";
#ifdef LL_OPUSFILE
        rc = decode_opus(data, size, sink, rate, channels, error_msg);
#else
        rc = fail(error_msg, "SoundFileOpusUnavailable", LLAUDIO_DEC_OPUS_UNAVAILABLE);
#endif
        break;
    case LLAudioFormat::Flac:
        clip.mFormatName = "FLAC";
        rc = decode_flac(data, size, sink, rate, channels, error_msg);
        break;
    case LLAudioFormat::Mp3:
        clip.mFormatName = "MP3";
        rc = decode_mp3(data, size, sink, rate, channels, error_msg);
        break;
    default:
        return fail(error_msg, "SoundFileUnknownFormat", LLAUDIO_DEC_UNKNOWN_FORMAT);
    }
    if (rc != LLAUDIO_DEC_OK)
    {
        return rc;
    }
    if (mono.empty())
    {
        return fail(error_msg, "SoundFileEmpty", LLAUDIO_DEC_EMPTY);
    }

    clip.mFormat = fmt;
    clip.mSourceRate = rate;
    clip.mSourceChannels = channels;
    if (rate == LLAUDIO_UPLOAD_SAMPLE_RATE)
    {
        clip.mSamples.swap(mono);
    }
    else
    {
        llaudio_resample_mono(mono, rate, LLAUDIO_UPLOAD_SAMPLE_RATE, clip.mSamples);
        std::vector<int16_t>().swap(mono);
        if (clip.mSamples.empty())
        {
            return fail(error_msg, "SoundFileEmpty", LLAUDIO_DEC_EMPTY);
        }
    }
    return LLAUDIO_DEC_OK;
}

int llaudio_decode_ogg_pcm(const uint8_t* data, size_t size, std::vector<int16_t>& pcm,
                           uint32_t& rate, uint32_t& channels, std::string& error_msg)
{
    error_msg.clear();
    pcm.clear();
    rate = channels = 0;
    if (!data || size == 0)
    {
        return fail(error_msg, "SoundFileEmpty", LLAUDIO_DEC_EMPTY);
    }
    if (llaudio_detect_format(data, size, "ogg") != LLAudioFormat::OggVorbis)
    {
        return fail(error_msg, "SoundFileUnknownFormat", LLAUDIO_DEC_UNKNOWN_FORMAT);
    }
    Sink sink(pcm, true);
    const int rc = decode_vorbis(data, size, sink, rate, channels, error_msg);
    if (rc != LLAUDIO_DEC_OK)
    {
        std::vector<int16_t>().swap(pcm);
        return rc;
    }
    if (pcm.empty())
    {
        return fail(error_msg, "SoundFileEmpty", LLAUDIO_DEC_EMPTY);
    }
    return LLAUDIO_DEC_OK;
}

#ifndef LL_AUDIODECODE_STANDALONE
int llaudio_decode_file(const std::string& fname, LLAudioClip& clip, std::string& error_msg)
{
    error_msg.clear();
    llifstream in(fname.c_str(), std::ios::binary);
    if (!in.is_open())
    {
        error_msg = "CannotUploadSoundFile";
        return LLAUDIO_DEC_SOURCE_OPEN_ERR;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff sz = in.tellg();
    in.seekg(0, std::ios::beg);
    if (sz < 0)
    {
        error_msg = "CannotUploadSoundFile";
        return LLAUDIO_DEC_SOURCE_OPEN_ERR;
    }
    if ((uint64_t)sz > LLAUDIO_DECODE_MAX_FILE_BYTES)
    {
        error_msg = "SoundFileTooBig";
        return LLAUDIO_DEC_FILE_TOO_BIG;
    }
    std::vector<uint8_t> bytes((size_t)sz);
    if (sz > 0)
    {
        in.read(reinterpret_cast<char*>(bytes.data()), sz);
        if ((std::streamoff)in.gcount() != sz)
        {
            error_msg = "CannotUploadSoundFile";
            return LLAUDIO_DEC_SOURCE_OPEN_ERR;
        }
    }
    std::string ext;
    const size_t dot = fname.rfind('.');
    if (dot != std::string::npos)
    {
        ext = fname.substr(dot + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    }
    return llaudio_decode_memory(bytes.data(), bytes.size(), ext, clip, error_msg);
}
#endif

// ---- resampler ---------------------------------------------------------------

namespace
{
uint64_t gcd64(uint64_t a, uint64_t b)
{
    while (b) { const uint64_t t = a % b; a = b; b = t; }
    return a;
}
}

void llaudio_resample_mono(const std::vector<int16_t>& in, uint32_t in_rate, uint32_t out_rate,
                           std::vector<int16_t>& out)
{
    out.clear();
    if (in.empty() || !in_rate || !out_rate) return;
    if (in_rate == out_rate)
    {
        out = in;
        return;
    }

    const uint64_t g = gcd64(in_rate, out_rate);
    const uint64_t M = in_rate / g;     // input step per L output samples
    const uint64_t L = out_rate / g;
    const uint64_t out_n = (uint64_t)in.size() * L / M;
    if (!out_n) return;

    // Phase table: at most kMaxPhases fractional positions (exact when L is small, as for
    // 48000 -> 44100 where L = 147).
    const uint64_t kMaxPhases = 2048;
    const uint64_t phases = std::min<uint64_t>(L, kMaxPhases);

    const double fc = std::min(1.0, (double)out_rate / (double)in_rate);   // cutoff vs input Nyquist
    const int half = (int)std::ceil(16.0 / fc);                            // taps each side
    const int taps = 2 * half;
    const double kPi = 3.14159265358979323846;

    std::vector<float> table((size_t)(phases * taps));
    for (uint64_t p = 0; p < phases; ++p)
    {
        const double frac = (double)p / (double)phases;
        double sum = 0.0;
        float* row = &table[(size_t)(p * taps)];
        for (int k = 0; k < taps; ++k)
        {
            const double dist = (double)(k - half + 1) - frac;    // input offset from output position
            const double x = dist * fc;
            const double sinc = (std::fabs(x) < 1e-12) ? 1.0 : std::sin(kPi * x) / (kPi * x);
            const double w = dist / (double)half;                 // window argument in [-1, 1]
            double win = 0.0;
            if (std::fabs(w) < 1.0)
            {
                win = 0.42 + 0.5 * std::cos(kPi * w) + 0.08 * std::cos(2.0 * kPi * w);   // Blackman
            }
            const double c = fc * sinc * win;
            row[k] = (float)c;
            sum += c;
        }
        if (sum != 0.0)
        {
            for (int k = 0; k < taps; ++k) row[k] = (float)(row[k] / sum);   // unity DC gain
        }
    }

    out.resize((size_t)out_n);
    const int64_t n_in = (int64_t)in.size();
    for (uint64_t i = 0; i < out_n; ++i)
    {
        const uint64_t num = i * M;                    // position in input = num / L
        const int64_t base = (int64_t)(num / L);
        const uint64_t rem = num % L;
        const uint64_t p = (rem * phases) / L;
        const float* row = &table[(size_t)(p * taps)];
        double acc = 0.0;
        const int64_t first = base - half + 1;
        if (first >= 0 && first + taps <= n_in)
        {
            for (int k = 0; k < taps; ++k) acc += (double)row[k] * (double)in[(size_t)(first + k)];
        }
        else
        {
            for (int k = 0; k < taps; ++k)
            {
                const int64_t idx = first + k;
                if (idx >= 0 && idx < n_in) acc += (double)row[k] * (double)in[(size_t)idx];
            }
        }
        const long v = std::lround(acc);
        out[(size_t)i] = (int16_t)std::max<long>(-32768, std::min<long>(32767, v));
    }
}

// ---- splitting ---------------------------------------------------------------

std::vector<LLAudioSplitPart> llaudio_plan_split(uint64_t total_frames, uint32_t rate, double seg_seconds)
{
    std::vector<LLAudioSplitPart> parts;
    if (!total_frames) return parts;
    uint64_t seg = (uint64_t)std::llround(seg_seconds * (double)rate);
    if (seg == 0) seg = total_frames;
    for (uint64_t pos = 0; pos < total_frames; pos += seg)
    {
        LLAudioSplitPart p;
        p.mStart = pos;
        p.mCount = std::min(seg, total_frames - pos);
        parts.push_back(p);
    }
    return parts;
}

std::string llaudio_split_part_name(const std::string& base, size_t index_1based, size_t count, size_t max_len)
{
    size_t digits = 2;
    for (size_t c = count; c >= 100; c /= 10) ++digits;
    std::string num = std::to_string(index_1based);
    while (num.size() < digits) num.insert(num.begin(), '0');
    const std::string suffix = "_" + num;
    std::string b = base;
    if (max_len > suffix.size() && b.size() > max_len - suffix.size())
    {
        b.resize(max_len - suffix.size());
    }
    return b + suffix;
}
