/**
 * @file llaudiofiledecode.h
 * @brief Decode WAV / Ogg Vorbis / Opus / MP3 / FLAC files to mono PCM for the sound uploader.
 *
 * Deliberately free of llcommon dependencies (plain std types) so it can be compiled
 * and tested standalone.
 *
 * Every input, including a user's .ogg, is decoded; the uploader then re-encodes the PCM
 * with our own Vorbis settings. Nothing user-supplied is ever passed through as-is.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_LLAUDIOFILEDECODE_H
#define LL_LLAUDIOFILEDECODE_H

#include <cstdint>
#include <string>
#include <vector>

// Sample rate the uploader encodes at (same value as LLVORBIS_CLIP_SAMPLE_RATE).
constexpr uint32_t LLAUDIO_UPLOAD_SAMPLE_RATE = 44100;
// The grid refuses sound assets longer than this.
constexpr double LLAUDIO_UPLOAD_LIMIT_SECONDS = 30.0;
// Hard cap on decoded audio (real decoded frames, not header claims): bounds memory and time.
constexpr double LLAUDIO_DECODE_MAX_SECONDS = 600.0;
// Highest source sample rate accepted.
constexpr uint32_t LLAUDIO_DECODE_MAX_SOURCE_RATE = 192000;
// Largest input file read into memory.
constexpr uint64_t LLAUDIO_DECODE_MAX_FILE_BYTES = 256ull * 1024 * 1024;

// Result codes. 0..11 intentionally match the LLVORBISENC_* values in llvorbisencode.h.
constexpr int LLAUDIO_DEC_OK                   = 0;
constexpr int LLAUDIO_DEC_SOURCE_OPEN_ERR      = 1;
constexpr int LLAUDIO_DEC_WAV_FORMAT_ERR       = 3;
constexpr int LLAUDIO_DEC_PCM_FORMAT_ERR       = 4;
constexpr int LLAUDIO_DEC_CHANNEL_ERR          = 7;
constexpr int LLAUDIO_DEC_SAMPLE_RATE_ERR      = 8;
constexpr int LLAUDIO_DEC_WORD_SIZE_ERR        = 9;
constexpr int LLAUDIO_DEC_TOO_LONG             = 10;
constexpr int LLAUDIO_DEC_CHUNK_SIZE_ERR       = 11;
constexpr int LLAUDIO_DEC_UNSUPPORTED_CODEC    = 12;
constexpr int LLAUDIO_DEC_CORRUPT              = 13;
constexpr int LLAUDIO_DEC_UNKNOWN_FORMAT       = 14;
constexpr int LLAUDIO_DEC_EMPTY                = 15;
constexpr int LLAUDIO_DEC_FILE_TOO_BIG         = 16;
constexpr int LLAUDIO_DEC_OPUS_UNAVAILABLE     = 17;

enum class LLAudioFormat
{
    Unknown,
    Wav,
    OggVorbis,
    Opus,
    Mp3,
    Flac
};

struct LLAudioClip
{
    LLAudioFormat mFormat = LLAudioFormat::Unknown;
    std::string   mFormatName;           // "WAV", "Ogg Vorbis", "Opus", "MP3", "FLAC"
    uint32_t      mSourceRate = 0;       // sample rate of the file
    uint32_t      mSourceChannels = 0;   // channels in the file
    // Decoded audio: mono (the uploader always encodes mono), 16-bit, at
    // LLAUDIO_UPLOAD_SAMPLE_RATE (resampled if the source rate differed).
    std::vector<int16_t> mSamples;

    uint64_t getFrames() const { return mSamples.size(); }
    double   getDuration() const { return double(mSamples.size()) / double(LLAUDIO_UPLOAD_SAMPLE_RATE); }
};

// True if Opus decoding was compiled in (LL_OPUSFILE).
bool llaudio_opus_available();

// Identify a file from its leading bytes (ext_lc is the lower-case extension without dot,
// used only as a hint for headerless MP3).
LLAudioFormat llaudio_detect_format(const uint8_t* data, size_t size, const std::string& ext_lc);

// Decode `fname` to a mono clip. Returns LLAUDIO_DEC_OK or an error code; on error
// `error_msg` receives the notification name to show (args: FILE).
// WAV input must already be 44100 Hz (existing uploader behaviour); compressed formats
// are resampled to 44100 Hz.
int llaudio_decode_file(const std::string& fname, LLAudioClip& clip, std::string& error_msg);

// Decode from memory (used by tests and llaudio_decode_file).
int llaudio_decode_memory(const uint8_t* data, size_t size, const std::string& ext_lc,
                          LLAudioClip& clip, std::string& error_msg);

// Decode an in-memory Ogg Vorbis file as it is: 16-bit PCM, interleaved, at the file's own
// rate and channel count (1 or 2), no resampling, under the same decoded-length cap. For
// bundled sounds played directly (the weather beds, #783); the caller reads the file.
int llaudio_decode_ogg_pcm(const uint8_t* data, size_t size, std::vector<int16_t>& pcm,
                           uint32_t& rate, uint32_t& channels, std::string& error_msg);

// Mono resampler (windowed sinc). Output length = floor(in.size() * out_rate / in_rate).
void llaudio_resample_mono(const std::vector<int16_t>& in, uint32_t in_rate,
                           uint32_t out_rate, std::vector<int16_t>& out);

// ---- splitting (#807) ------------------------------------------------------

struct LLAudioSplitPart
{
    uint64_t mStart = 0;   // first frame
    uint64_t mCount = 0;   // frame count
};

// Cut `total_frames` into consecutive parts of floor(seg_seconds * rate) frames, the last
// holding the remainder. Exact sample boundaries: part[i].mStart + part[i].mCount ==
// part[i+1].mStart. If total_frames fits within one segment, returns a single part.
std::vector<LLAudioSplitPart> llaudio_plan_split(uint64_t total_frames, uint32_t rate, double seg_seconds);

// "base_01" ... ; digits = max(2, digits of count). The base is truncated (UTF-8 safe for
// ASCII names) so the whole name fits in max_len bytes.
std::string llaudio_split_part_name(const std::string& base, size_t index_1based, size_t count, size_t max_len);

#endif // LL_LLAUDIOFILEDECODE_H
