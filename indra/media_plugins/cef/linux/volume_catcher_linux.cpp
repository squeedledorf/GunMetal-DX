/**
 * @file volume_catcher.cpp
 * @brief Linux volume catcher which will pick an implementation to use
 *
 * @cond
 * $LicenseInfo:firstyear=2010&license=viewerlgpl$
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
 * @endcond
 */

#include "volume_catcher_linux.h"

#include <cstdlib>
#include <unistd.h>

VolumeCatcher::VolumeCatcher()
{
}

void VolumeCatcher::onEnablePipeWireVolumeCatcher(bool enable)
{
    if (pimpl != nullptr)
        return;

    if (enable)
    {
        LL_DEBUGS() << "volume catcher using pipewire" << LL_ENDL;
        pimpl = new VolumeCatcherPipeWire();
    }
    else
    {
        LL_DEBUGS() << "volume catcher using pulseaudio" << LL_ENDL;
        pimpl = new VolumeCatcherPulseAudio();
    }
}

VolumeCatcher::~VolumeCatcher()
{
    if (pimpl != nullptr)
    {
        delete pimpl;
        pimpl = nullptr;
    }
}

// The viewer may never send enable_pipewire_volume_catcher; without a backend every
// volume change (including the master mute) was silently dropped. Prefer PipeWire
// when its library loads and a PipeWire server socket exists; otherwise PulseAudio,
// which also covers pipewire-pulse setups and the Flatpak sandbox (pulseaudio socket).
void VolumeCatcher::ensureImpl()
{
    if (pimpl != nullptr)
        return;

    bool use_pipewire = false;
    if (!getenv("FLATPAK_ID"))
    {
        std::string dir;
        if (const char* rt = getenv("PIPEWIRE_RUNTIME_DIR"))
            dir = rt;
        else if (const char* xdg = getenv("XDG_RUNTIME_DIR"))
            dir = xdg;
        const char* remote = getenv("PIPEWIRE_REMOTE");
        std::string sock = dir + "/" + (remote ? remote : "pipewire-0");
        use_pipewire = !dir.empty() && access(sock.c_str(), F_OK) == 0;
    }

    if (use_pipewire)
    {
        VolumeCatcherPipeWire* pw = new VolumeCatcherPipeWire();
        if (pw->mGotSyms)
        {
            LL_INFOS() << "volume catcher using pipewire" << LL_ENDL;
            pimpl = pw;
            return;
        }
        delete pw;
    }
    LL_INFOS() << "volume catcher using pulseaudio" << LL_ENDL;
    pimpl = new VolumeCatcherPulseAudio();
}

void VolumeCatcher::setVolume(F32 volume)
{
    ensureImpl();
    if (pimpl != nullptr) {
        pimpl->setVolume(volume);
    }
}

void VolumeCatcher::setPan(F32 pan)
{
    ensureImpl();
    if (pimpl != nullptr)
        pimpl->setPan(pan);
}

void VolumeCatcher::pump()
{
    if (pimpl != nullptr)
        pimpl->pump();
}
