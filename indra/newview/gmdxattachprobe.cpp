/**
 * @file gmdxattachprobe.cpp
 * @brief Throttled property probes of other residents' attachments.
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

#include "llviewerprecompiledheaders.h"

#include "gmdxattachprobe.h"

#include "fssoundemitterblacklist.h"
#include "llcallbacklist.h"
#include "llderenderlist.h"
#include "llframetimer.h"
#include "llselectmgr.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatar.h"
#include "llworld.h"

#include <deque>

namespace
{
    // Per wearer: this many asks per window. The match is usually among the first few
    // attachments asked about, and the asks stop as soon as the wearer's entries are matched.
    constexpr S32 PROBES_PER_WINDOW = 4;
    constexpr F64 PROBE_WINDOW = 10.0;

    struct Pending
    {
        LLUUID object_id;
        U64    region_handle;
        U32    local_id;
    };

    struct Wearer
    {
        F64                 window_start = 0.0;
        S32                 sent = 0;
        std::deque<Pending> queue;
    };

    uuid_set_t               sAsked;     // asked or waiting, this session
    std::map<LLUUID, Wearer> sWearers;
    bool                     sDraining = false;

    bool wanted(const LLUUID& wearer_id, const LLUUID& object_id)
    {
        return (LLDerenderList::instanceExists() && LLDerenderList::instance().wantsProperties(wearer_id, object_id))
            || FSSoundEmitterBlacklist::instance().wantsProperties(wearer_id, object_id);
    }

    // Sends what the wearer's window has room for. True when nothing is left waiting.
    bool send(const LLUUID& wearer_id, Wearer& wearer, F64 now)
    {
        if (now - wearer.window_start >= PROBE_WINDOW)
        {
            wearer.window_start = now;
            wearer.sent = 0;
        }
        while (!wearer.queue.empty() && wearer.sent < PROBES_PER_WINDOW)
        {
            const Pending pending = wearer.queue.front();
            wearer.queue.pop_front();
            if (!wanted(wearer_id, pending.object_id))
            {
                continue; // matched meanwhile
            }
            // Still here: ask where it is now. Gone from view: only a derendered one (killed
            // here, still in the sim) is worth asking about where it was.
            LLViewerRegion* regionp = nullptr;
            U32 local_id = pending.local_id;
            LLViewerObject* objectp = gObjectList.findObject(pending.object_id);
            if (objectp && !objectp->isDead())
            {
                regionp = objectp->getRegion();
                local_id = objectp->getLocalID();
            }
            else if (LLDerenderList::instanceExists() && LLDerenderList::instance().isDerendered(pending.object_id))
            {
                regionp = LLWorld::getInstance()->getRegionFromHandle(pending.region_handle);
            }
            if (!regionp)
            {
                continue;
            }
            LLSelectMgr::getInstance()->requestObjectPropertiesViaSelect(regionp, local_id);
            ++wearer.sent;
        }
        return wearer.queue.empty();
    }

    bool drain()
    {
        const F64 now = LLFrameTimer::getTotalSeconds();
        for (auto it = sWearers.begin(); it != sWearers.end(); )
        {
            const bool done = send(it->first, it->second, now);
            it = (done && now - it->second.window_start >= PROBE_WINDOW) ? sWearers.erase(it) : std::next(it);
        }
        sDraining = !sWearers.empty();
        return !sDraining;
    }
}

namespace GMDXAttachProbe
{
    void onAttachmentAdded(LLVOAvatar* wearer, LLViewerObject* attachment)
    {
        if (!wearer || wearer->isSelf() || !attachment || attachment->isDead())
        {
            return;
        }
        const LLUUID& wearer_id = wearer->getID();
        const LLUUID& object_id = attachment->getID();
        if (!wanted(wearer_id, object_id) || !sAsked.insert(object_id).second)
        {
            return;
        }
        const LLViewerRegion* regionp = attachment->getRegion();
        if (!regionp)
        {
            return;
        }

        Wearer& state = sWearers[wearer_id];
        state.queue.push_back({ object_id, regionp->getHandle(), attachment->getLocalID() });
        if (!send(wearer_id, state, LLFrameTimer::getTotalSeconds()) && !sDraining)
        {
            sDraining = true;
            doPeriodically(drain, 1.f);
        }
    }
}
