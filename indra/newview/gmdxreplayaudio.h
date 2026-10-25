/**
 * @file gmdxreplayaudio.h
 * @brief GunMetal-DX instant replay: the viewer's own sound, Opus-encoded for the replay's audio track.
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

// <GMDX> INSTANT REPLAY AUDIO. Records what the viewer itself plays -- world sounds, UI, music and
// media streams, voice -- never the microphone and never other programs:
//  - Windows: WASAPI process loopback of this process and its children (Windows 10 2004 or later).
//  - Linux: a monitor of each PulseAudio / PipeWire playback stream that belongs to this process or
//    its children (libpulse, loaded at run time; pipewire-pulse serves it on PipeWire systems).
// A mixer thread turns that into 20 ms Opus packets on the replay's clock, kept for the replay length.
// Plain std + OS APIs (no viewer types), so the standalone test program can drive it.

#pragma once

#include "vkgfx/gmdxreplayvk.h"   // GMDXReplayAudioClip

#include <cstdint>
#include <memory>
#include <string>

class GMDXReplayAudio
{
public:
    GMDXReplayAudio();
    ~GMDXReplayAudio();   // stops

    // Starts capturing. Times are the replay clock: std::chrono::steady_clock in seconds.
    // Problems show up later via takeError.
    void start();
    void stop();

    void setKeepSeconds(uint32_t seconds);
    // The packets covering the last `seconds` (plus a little before, for the writer to trim).
    GMDXReplayAudioClip clip(double nowSec, uint32_t seconds) const;

    // A problem worth telling the user about (once): true when there was one.
    bool takeError(std::string& message);
    std::string describe() const;   // log line

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
