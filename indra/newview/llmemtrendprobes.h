/**
 * @file llmemtrendprobes.h
 * @brief The viewer's MemTrend probes (#883): long-lived containers whose size the
 *        periodic leak report tracks. See llcommon/llmemtrend.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
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

#ifndef LL_LLMEMTRENDPROBES_H
#define LL_LLMEMTRENDPROBES_H

// Registers the viewer-side probes once (main thread). A subsystem that owns a store can
// also call LLMemTrend::registerProbe itself; this file covers the shared / LL ones.
void registerMemTrendProbes();

#endif // LL_LLMEMTRENDPROBES_H
