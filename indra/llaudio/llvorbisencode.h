/**
 * @file vorbisencode.h
 * @brief Vorbis encoding routine routine for Indra.
 *
 * $LicenseInfo:firstyear=2000&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
 *
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#ifndef LL_VORBISENCODE_H
#define LL_VORBISENCODE_H

const S32 LLVORBISENC_NOERR                        = 0; // no error
const S32 LLVORBISENC_SOURCE_OPEN_ERR              = 1; // error opening source
const S32 LLVORBISENC_DEST_OPEN_ERR                = 2; // error opening destination
const S32 LLVORBISENC_WAV_FORMAT_ERR               = 3; // not a WAV
const S32 LLVORBISENC_PCM_FORMAT_ERR               = 4; // not a PCM
const S32 LLVORBISENC_MONO_ERR                     = 5; // can't do mono
const S32 LLVORBISENC_STEREO_ERR                   = 6; // can't do stereo
const S32 LLVORBISENC_MULTICHANNEL_ERR             = 7; // can't do stereo
const S32 LLVORBISENC_UNSUPPORTED_SAMPLE_RATE      = 8; // unsupported sample rate
const S32 LLVORBISENC_UNSUPPORTED_WORD_SIZE        = 9; // unsupported word size
const S32 LLVORBISENC_CLIP_TOO_LONG                = 10; // source file is too long
const S32 LLVORBISENC_CHUNK_SIZE_ERR               = 11; // chunk size is wrong

const F32 LLVORBIS_CLIP_MAX_TIME                               = 60.0f;
const U8  LLVORBIS_CLIP_MAX_CHANNELS                   = 2;
const U32 LLVORBIS_CLIP_SAMPLE_RATE                            = 44100;
const U32 LLVORBIS_CLIP_MAX_SAMPLES_PER_CHANNEL        = (U32)(LLVORBIS_CLIP_MAX_TIME * LLVORBIS_CLIP_SAMPLE_RATE);
const U32 LLVORBIS_CLIP_MAX_SAMPLES                            = LLVORBIS_CLIP_MAX_SAMPLES_PER_CHANNEL * LLVORBIS_CLIP_MAX_CHANNELS;
const size_t LLVORBIS_CLIP_MAX_SAMPLE_DATA             = LLVORBIS_CLIP_MAX_SAMPLES * 2; // 2 = 16-bit

// Treat anything this long as a bad asset. A little fudge factor at the end:
// Make that a lot of fudge factor. We're allowing 30 sec for now - 3x legal upload
const size_t LLVORBIS_CLIP_REJECT_SAMPLES              = LLVORBIS_CLIP_MAX_SAMPLES * 3;
const size_t LLVORBIS_CLIP_REJECT_SIZE                 = LLVORBIS_CLIP_MAX_SAMPLE_DATA * 3;

const S32 LLVORBISENC_UNSUPPORTED_CODEC            = 12; // codec not supported (e.g. unsupported codec inside an Ogg)
const S32 LLVORBISENC_CORRUPT_FILE                 = 13; // file is damaged / could not be decoded

// Legacy WAV-only path (unchanged behaviour; clip limit LLVORBIS_CLIP_MAX_TIME).
S32 check_for_invalid_wav_formats(const std::string& in_fname, std::string& error_msg);
// Same checks with a caller-chosen maximum length (the sound uploader lets longer clips
// through so they can be offered for splitting).
S32 check_for_invalid_wav_formats(const std::string& in_fname, std::string& error_msg, F32 max_clip_seconds);
S32 encode_vorbis_file(const std::string& in_fname, const std::string& out_fname);

// Encode already-decoded mono 16-bit PCM (see llaudiofiledecode.h) to Ogg Vorbis with the
// same encoder settings as encode_vorbis_file. fade_in_frames / fade_out_frames apply a
// linear ramp at the start / end (0 = plain cut).
S32 encode_vorbis_pcm(const S16* mono_samples, size_t frame_count, U32 sample_rate,
                      const std::string& out_fname, size_t fade_in_frames = 0, size_t fade_out_frames = 0);

#endif

