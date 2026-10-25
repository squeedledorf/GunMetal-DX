/**
 * @file sldiscordpresence.h
 * @brief Discord Rich Presence (#205), spoken directly over Discord's local IPC
 *        socket / named pipe. No Discord SDK and no third-party library.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
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
 * $/LicenseInfo$
 */

#ifndef SL_SLDISCORDPRESENCE_H
#define SL_SLDISCORDPRESENCE_H

// Everything runs on the main thread: the IPC endpoint is non-blocking and is
// serviced at most once per second from LLAppViewer::idle(), so Discord being
// absent, slow or hung never stalls a frame.
namespace SLDiscordPresence
{
    // Rich Presence art asset keys. Upload images under exactly these names in
    // the Discord developer portal (application -> Rich Presence -> Art Assets).
    constexpr const char* LARGE_IMAGE_KEY = "slipstream_logo";

    // Discord accepts at most ~5 activity updates per 20 s; we send at most one
    // per this many seconds, and only when the content changed.
    constexpr double MIN_UPDATE_INTERVAL = 15.0;

    // Call every frame; self rate-limited to one tick per second. Connects /
    // reconnects (with backoff), builds the activity from the current settings
    // and agent state and sends it when it changed.
    void idle();

    // Clears the activity and closes the connection; nothing is sent after it.
    // Idempotent. Called from LLAppViewer::disconnectViewer() (logout / quit).
    void shutdown();
}

#endif // SL_SLDISCORDPRESENCE_H
