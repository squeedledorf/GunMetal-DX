/**
 * @file sldatausage.h
 * @brief Per-session network data accounting by category (#529).
 *
 * The counters live where the bytes arrive: llcorehttp keeps per-policy-class
 * transfer totals (LLCore::HttpRequest::getTransferStats), LLPacketRing keeps UDP
 * session totals and every LLMessageTemplate its received wire bytes. This module
 * adds the cache-effectiveness counters, samples media / voice activity once a second
 * and turns all of it into the "DataUsage:" log summary (every 30 minutes and at
 * exit), the bug report System block and the Help > About line.
 *
 * Overhead: relaxed atomics / plain main-thread integers on the counting paths, no
 * locks, no per-packet logging. The summaries walk the counters on the main thread.
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

#ifndef SL_SLDATAUSAGE_H
#define SL_SLDATAUSAGE_H

#include "stdtypes.h"

#include <string>

class LLViewerObject;

namespace SLDataUsage
{
    // ---- counting (cheap; safe from any thread) ----

    // Texture fetch worker decided between the local disk cache and the network
    // (texture fetch threads). hit = the cache had enough data for the wanted discard.
    void noteTextureCache(bool hit);

    // Object cache probe for one ObjectUpdateCached entry (main thread).
    // crc_miss = the object was cached but stale, so it is requested again.
    void noteObjectCacheProbe(bool hit, bool crc_miss);

    // A region's object cache was read at region handshake (main thread). usable =
    // the cache file was accepted and not empty; entries = objects it brought in.
    void noteRegionCacheLoad(bool usable, U32 entries);

    // One object update block was applied (main thread): counted per object so the
    // summary can name what the simulator keeps updating. update_type is EObjectUpdateType.
    void noteObjectUpdate(const LLViewerObject* object, S32 update_type);

    // ---- reporting (main thread) ----

    // Once per frame from LLAppViewer::idle(): samples media / voice once a second and
    // logs the summary every 30 minutes.
    void idleTick();

    // The full multi-line "DataUsage:" INFO summary; 'when' names the occasion.
    void logSummary(const std::string& when);

    // One line for Help > About: what was downloaded this session.
    std::string aboutText();

    // Short values (each <= 200 characters, the bug report relay's per-field limit)
    // for the bug report System block: 0 totals, 1 HTTP by category, 2 top UDP
    // messages, 3 cache hit rates + media / voice.
    std::string reportText(int part);
}

#endif // SL_SLDATAUSAGE_H
