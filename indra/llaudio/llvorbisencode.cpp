/**
 * @file vorbisencode.cpp
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

#include "linden_common.h"

#include <algorithm>
#include <functional>

#include "vorbis/vorbisenc.h"

#include "llvorbisencode.h"
#include "llerror.h"
#include "llrand.h"
#include "llmath.h"
#include "llapr.h"

static const long VORBIS_READ_BUFFER = 1024;   // samples handed to the encoder per iteration

S32 check_for_invalid_wav_formats(const std::string& in_fname, std::string& error_msg)
{
    return check_for_invalid_wav_formats(in_fname, error_msg, LLVORBIS_CLIP_MAX_TIME);
}

S32 check_for_invalid_wav_formats(const std::string& in_fname, std::string& error_msg, F32 max_clip_seconds)
{
    U16 num_channels = 0;
    U32 sample_rate = 0;
    U32 bits_per_sample = 0;
    U32 physical_file_size = 0;
    U32 chunk_length = 0;
    U32 raw_data_length = 0;
    U32 bytes_per_sec = 0;
    bool uncompressed_pcm = false;

    unsigned char wav_header[44];       /*Flawfinder: ignore*/

    error_msg.clear();

    //********************************
    LLAPRFile infile ;
    infile.open(in_fname,LL_APR_RB);
    //********************************
    if (!infile.getFileHandle())
    {
        error_msg = "CannotUploadSoundFile";
        return(LLVORBISENC_SOURCE_OPEN_ERR);
    }

    infile.read(wav_header, 44);
    physical_file_size = infile.seek(APR_END,0);

    if (strncmp((char *)&(wav_header[0]),"RIFF",4))
    {
        error_msg = "SoundFileNotRIFF";
        return(LLVORBISENC_WAV_FORMAT_ERR);
    }

    if (strncmp((char *)&(wav_header[8]),"WAVE",4))
    {
        error_msg = "SoundFileNotRIFF";
        return(LLVORBISENC_WAV_FORMAT_ERR);
    }

    // parse the chunks

    U32 file_pos = 12;  // start at the first chunk (usually fmt but not always)

    while ((file_pos + 8)< physical_file_size)
    {
        infile.seek(APR_SET,file_pos);
        infile.read(wav_header, 44);

        chunk_length = ((U32) wav_header[7] << 24)
            + ((U32) wav_header[6] << 16)
            + ((U32) wav_header[5] << 8)
            + wav_header[4];

        if (chunk_length > physical_file_size - file_pos - 4)
        {
            infile.close();
            error_msg = "SoundFileInvalidChunkSize";
            return(LLVORBISENC_CHUNK_SIZE_ERR);
        }

//      LL_INFOS() << "chunk found: '" << wav_header[0] << wav_header[1] << wav_header[2] << wav_header[3] << "'" << LL_ENDL;

        if (!(strncmp((char *)&(wav_header[0]),"fmt ",4)))
        {
            if ((wav_header[8] == 0x01) && (wav_header[9] == 0x00))
            {
                uncompressed_pcm = true;
            }
            num_channels = ((U16) wav_header[11] << 8) + wav_header[10];
            sample_rate = ((U32) wav_header[15] << 24)
                + ((U32) wav_header[14] << 16)
                + ((U32) wav_header[13] << 8)
                + wav_header[12];
            bits_per_sample = ((U16) wav_header[23] << 8) + wav_header[22];
            bytes_per_sec = ((U32) wav_header[19] << 24)
                + ((U32) wav_header[18] << 16)
                + ((U32) wav_header[17] << 8)
                + wav_header[16];
        }
        else if (!(strncmp((char *)&(wav_header[0]),"data",4)))
        {
            raw_data_length = chunk_length;
        }
        file_pos += (chunk_length + 8);
        chunk_length = 0;
    }
    //****************
    infile.close();
    //****************

    if (!uncompressed_pcm)
    {
         error_msg = "SoundFileNotPCM";
          return(LLVORBISENC_PCM_FORMAT_ERR);
    }

    if ((num_channels < 1) || (num_channels > LLVORBIS_CLIP_MAX_CHANNELS))
    {
        error_msg = "SoundFileInvalidChannelCount";
        return(LLVORBISENC_MULTICHANNEL_ERR);
    }

    if (sample_rate != LLVORBIS_CLIP_SAMPLE_RATE)
    {
        error_msg = "SoundFileInvalidSampleRate";
        return(LLVORBISENC_UNSUPPORTED_SAMPLE_RATE);
    }

    if ((bits_per_sample != 16) && (bits_per_sample != 8))
    {
        error_msg = "SoundFileInvalidWordSize";
        return(LLVORBISENC_UNSUPPORTED_WORD_SIZE);
    }

    if (!raw_data_length)
    {
        error_msg = "SoundFileInvalidHeader";
        return(LLVORBISENC_CLIP_TOO_LONG);
    }

    F32 clip_length = (F32)raw_data_length/(F32)bytes_per_sec;

    if (clip_length > max_clip_seconds)
    {
        error_msg = "SoundFileInvalidTooLong";
        return(LLVORBISENC_CLIP_TOO_LONG);
    }

    return(LLVORBISENC_NOERR);
}

// Shared Vorbis encoder core. `fill(dst, max)` writes up to `max` mono float samples
// (-1..1) into dst and returns the count; 0 means end of input. Everything the viewer
// uploads as a sound goes through here, so the encode settings live in exactly one place.
static S32 encode_vorbis_core(U32 sample_rate, const std::string& out_fname,
                              const std::function<long(float*, long)>& fill)
{
    ogg_stream_state os; /* take physical pages, weld into a logical stream of packets */
    ogg_page         og; /* one Ogg bitstream page.  Vorbis packets are inside */
    ogg_packet       op; /* one raw packet of data for decode */

    vorbis_info      vi; /* struct that stores all the static vorbis bitstream settings */
    vorbis_comment   vc; /* struct that stores all the user comments */

    vorbis_dsp_state vd; /* central working state for the packet->PCM decoder */
    vorbis_block     vb; /* local working space for packet->PCM decode */

    int eos=0;
    int result;

    LLAPRFile outfile ;
    outfile.open(out_fname,LL_APR_WPB);
    if (!outfile.getFileHandle())
    {
        LL_WARNS() << "Couldn't open upload sound file for writing: " << out_fname
            << LL_ENDL;
        return(LLVORBISENC_DEST_OPEN_ERR);
    }

     /********** Encode setup ************/

     /* choose an encoding mode */
     /* (mode 0: 44kHz stereo uncoupled, roughly 128kbps VBR) */
     vorbis_info_init(&vi);

     // always encode to mono

     // SL-52913 & SL-53779 determined this quality level to be our 'good
     // enough' general-purpose quality level with a nice low bitrate.
     // Equivalent to oggenc -q0.5
     F32 quality = 0.05f;

     if (vorbis_encode_init_vbr(&vi, /* num_channels */ 1 ,sample_rate, quality))
    {
        LL_WARNS() << "unable to initialize vorbis codec at quality " << quality << LL_ENDL;
        return(LLVORBISENC_DEST_OPEN_ERR);
    }

     /* add a comment */
     vorbis_comment_init(&vc);

     /* set up the analysis state and auxiliary encoding storage */
     vorbis_analysis_init(&vd,&vi);
     vorbis_block_init(&vd,&vb);

     /* set up our packet->stream encoder */
     /* pick a random serial number; that way we can more likely build
        chained streams just by concatenation */
     ogg_stream_init(&os, ll_rand());

     /* Vorbis streams begin with three headers; the initial header (with
        most of the codec setup parameters) which is mandated by the Ogg
        bitstream spec.  The second header holds any comment fields.  The
        third header holds the bitstream codebook.  We merely need to
        make the headers, then pass them to libvorbis one at a time;
        libvorbis handles the additional Ogg bitstream constraints */

     {
         ogg_packet header;
         ogg_packet header_comm;
         ogg_packet header_code;

         vorbis_analysis_headerout(&vd,&vc,&header,&header_comm,&header_code);
         ogg_stream_packetin(&os,&header); /* automatically placed in its own
                                              page */
         ogg_stream_packetin(&os,&header_comm);
         ogg_stream_packetin(&os,&header_code);

         /* We don't have to write out here, but doing so makes streaming
          * much easier, so we do, flushing ALL pages. This ensures the actual
          * audio data will start on a new page
          */
         while(!eos){
             int result=ogg_stream_flush(&os,&og);
             if(result==0)break;
             outfile.write(og.header, og.header_len);
             outfile.write(og.body, og.body_len);
         }

     }


     while(!eos)
     {
         /* expose the buffer to submit data */
         float **buffer=vorbis_analysis_buffer(&vd,VORBIS_READ_BUFFER);

         long samples = fill(buffer[0], VORBIS_READ_BUFFER);

         if (samples <= 0)
         {
             /* end of file.  Tell the library we're at end of stream so that it can
                handle the last frame and mark end of stream in the output properly */
             vorbis_analysis_wrote(&vd,0);
         }
         else
         {
             /* tell the library how much we actually submitted */
             vorbis_analysis_wrote(&vd,samples);
         }

         /* vorbis does some data preanalysis, then divvies up blocks for
            more involved (potentially parallel) processing.  Get a single
            block for encoding now */
         while(vorbis_analysis_blockout(&vd,&vb)==1)
         {

             /* analysis */
            /* Do the main analysis, creating a packet */
            vorbis_analysis(&vb, NULL);
            vorbis_bitrate_addblock(&vb);

            while(vorbis_bitrate_flushpacket(&vd, &op))
            {

             /* weld the packet into the bitstream */
             ogg_stream_packetin(&os,&op);

             /* write out pages (if any) */
             while(!eos)
             {
                 result = ogg_stream_pageout(&os,&og);

                 if(result==0)
                    break;

                 outfile.write(og.header, og.header_len);
                 outfile.write(og.body, og.body_len);

                 /* this could be set above, but for illustrative purposes, I do
                    it here (to show that vorbis does know where the stream ends) */

                 if(ogg_page_eos(&og))
                    eos=1;

             }
            }
         }
     }

     /* clean up and exit.  vorbis_info_clear() must be called last */

     ogg_stream_clear(&os);
     vorbis_block_clear(&vb);
     vorbis_dsp_clear(&vd);
     vorbis_comment_clear(&vc);
     vorbis_info_clear(&vi);

     LL_INFOS() << "Vorbis encoding: Done." << LL_ENDL;

     return(LLVORBISENC_NOERR);
}

S32 encode_vorbis_file(const std::string& in_fname, const std::string& out_fname)
{
    unsigned char readbuffer[VORBIS_READ_BUFFER*4+44];   /* out of the data segment, not the stack */  /*Flawfinder: ignore*/

    U16 num_channels = 0;
    U32 sample_rate = 0;
    U32 bits_per_sample = 0;

    S32 format_error = 0;
    std::string error_msg;
    if ((format_error = check_for_invalid_wav_formats(in_fname, error_msg)))
    {
        LL_WARNS() << error_msg << ": " << in_fname << LL_ENDL;
        return(format_error);
    }

    unsigned char wav_header[44];   /*Flawfinder: ignore*/

    S32 data_left = 0;

    LLAPRFile infile ;
    infile.open(in_fname,LL_APR_RB);
    if (!infile.getFileHandle())
    {
        LL_WARNS() << "Couldn't open temporary ogg file for writing: " << in_fname
            << LL_ENDL;
        return(LLVORBISENC_SOURCE_OPEN_ERR);
    }

     // parse the chunks
     U32 chunk_length = 0;
     U32 file_pos = 12;  // start at the first chunk (usually fmt but not always)

     while (infile.eof() != APR_EOF)
     {
         infile.seek(APR_SET,file_pos);
         infile.read(wav_header, 44);

         chunk_length = ((U32) wav_header[7] << 24)
             + ((U32) wav_header[6] << 16)
             + ((U32) wav_header[5] << 8)
             + wav_header[4];

         if (!(strncmp((char *)&(wav_header[0]),"fmt ",4)))
         {
             num_channels = ((U16) wav_header[11] << 8) + wav_header[10];
             sample_rate = ((U32) wav_header[15] << 24)
                 + ((U32) wav_header[14] << 16)
                 + ((U32) wav_header[13] << 8)
                 + wav_header[12];
             bits_per_sample = ((U16) wav_header[23] << 8) + wav_header[22];
         }
         else if (!(strncmp((char *)&(wav_header[0]),"data",4)))
         {
             infile.seek(APR_SET,file_pos+8);
             // leave the file pointer at the beginning of the data chunk data
             data_left = chunk_length;
             break;
         }
         file_pos += (chunk_length + 8);
         chunk_length = 0;
     }

     // Feed the shared encoder from the WAV data (unchanged conversion maths).
     auto fill = [&](float* dst, long max_samples) -> long
     {
         long bytes_per_sample = bits_per_sample/8;

         long bytes=(long)infile.read(readbuffer,llclamp((S32)(max_samples*num_channels*bytes_per_sample),0,data_left)); /* stereo hardwired here */

         if (bytes==0)
         {
             return 0;
         }

         long i;
         long samples;
         int temp;

         data_left -= bytes;
         i = 0;
         samples = bytes / (num_channels * bytes_per_sample);

         if (num_channels == 2)
         {
             if (bytes_per_sample == 2)
             {
                 /* uninterleave samples */
                 for(i=0; i<samples ;i++)
                 {
                     temp =  ((signed char *)readbuffer)[i*4+1];    /*Flawfinder: ignore*/
                     temp += ((signed char *)readbuffer)[i*4+3];    /*Flawfinder: ignore*/
                     temp <<= 8;
                     temp += readbuffer[i*4];
                     temp += readbuffer[i*4+2];

                     dst[i] = ((float)temp) / 65536.f;
                 }
             }
             else // presume it's 1 byte per which is unsigned (F#@%ing wav "standard")
             {
                 /* uninterleave samples */
                 for(i=0; i<samples ;i++)
                 {
                     temp  = readbuffer[i*2+0];
                     temp += readbuffer[i*2+1];
                     temp -= 256;
                     dst[i] = ((float)temp) / 256.f;
                 }
             }
         }
         else if (num_channels == 1)
         {
             if (bytes_per_sample == 2)
             {
                 for(i=0; i < samples ;i++)
                 {
                     temp = ((signed char*)readbuffer)[i*2+1];
                     temp <<= 8;
                     temp += readbuffer[i*2];
                     dst[i] = ((float)temp) / 32768.f;
                 }
             }
             else // presume it's 1 byte per which is unsigned (F#@%ing wav "standard")
             {
                 for(i=0; i < samples ;i++)
                 {
                     temp = readbuffer[i];
                     temp -= 128;
                     dst[i] = ((float)temp) / 128.f;
                 }
             }
         }
         return i;
     };

     return encode_vorbis_core(sample_rate, out_fname, fill);
}

S32 encode_vorbis_pcm(const S16* mono_samples, size_t frame_count, U32 sample_rate,
                      const std::string& out_fname, size_t fade_in_frames, size_t fade_out_frames)
{
    if (!mono_samples || frame_count == 0)
    {
        return LLVORBISENC_SOURCE_OPEN_ERR;
    }
    if (fade_in_frames > frame_count) fade_in_frames = frame_count;
    if (fade_out_frames > frame_count) fade_out_frames = frame_count;

    size_t pos = 0;
    auto fill = [&](float* dst, long max_samples) -> long
    {
        const size_t n = std::min((size_t)max_samples, frame_count - pos);
        for (size_t i = 0; i < n; ++i)
        {
            const size_t f = pos + i;
            float v = (float)mono_samples[f] / 32768.f;
            if (f < fade_in_frames)
            {
                v *= (float)f / (float)fade_in_frames;
            }
            const size_t from_end = frame_count - 1 - f;
            if (from_end < fade_out_frames)
            {
                v *= (float)from_end / (float)fade_out_frames;
            }
            dst[i] = v;
        }
        pos += n;
        return (long)n;
    };

    return encode_vorbis_core(sample_rate, out_fname, fill);
}
