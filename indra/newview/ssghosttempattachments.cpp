/**
 * @file ssghosttempattachments.cpp
 * @brief Ghost temp attachment cleanup after a teleport (LLAppearanceMgr::checkForGhostTempAttachments).
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

// <SS:Nexii> Ghost temp attachment cleanup after a teleport (SoapStorm 75bfec4d68).
// Temp attachments from an experience that the new region does not allow are
// dropped by the simulator but can survive viewer-side as ghosts that can no
// longer be detached. Once the agent settled, remove them from the root down
// through the linkset.

#include "llviewerprecompiledheaders.h"
#include "llappearancemgr.h"

#include "llagent.h"
#include "llagentwearables.h"
#include "llapp.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lleventcoro.h"
#include "llselectmgr.h"
#include "llviewerjointattachment.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llvoavatarself.h"

namespace
{
    // Time to let the agent settle in the new region after the teleport
    // completed before the region's attachment report can be trusted. The
    // simulator needs this time to rez the attachments it kept.
    const F32 GHOST_TEMP_ATTACHMENT_SETTLE_DELAY = 15.0F;
    // Time between the two attachment report fetches; an attachment only
    // counts as a ghost if it is missing from both reports, so attachments
    // that merely arrive late are not removed by mistake.
    const F32 GHOST_TEMP_ATTACHMENT_CONFIRM_DELAY = 5.0F;
    // Upper bound for waiting on a teleport that is still in flight.
    const F32 GHOST_TEMP_ATTACHMENT_MAX_WAIT = 60.0F;

    bool sGhostTempAttachmentCheckRunning = false;

    // Fetch the region's "AttachmentResources" report and collect the object IDs of
    // the attachments the simulator currently believes the agent is wearing. This
    // is the same report the temporary attachments panel uses to resolve names, so
    // a temp attachment absent from it is exactly one whose name lookup would fail.
    bool get_region_attachment_ids(const std::string& url, std::set<LLUUID>& attachment_ids)
    {
        LLCore::HttpRequest::policy_t httpPolicy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
        LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t httpAdapter = std::make_shared<LLCoreHttpUtil::HttpCoroutineAdapter>(
                    "get_region_attachment_ids", httpPolicy);
        LLCore::HttpRequest::ptr_t httpRequest = std::make_shared<LLCore::HttpRequest>();

        LLSD result = httpAdapter->getAndSuspend(httpRequest, url);

        LLSD httpResults = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
        LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(httpResults);
        if (!status)
        {
            LL_WARNS("Avatar") << "Could not retrieve attachment resources for the ghost temp attachment check."
                               << LL_ENDL;
            return false;
        }

        const LLSD& attachments = result["attachments"];
        for (LLSD::array_const_iterator it = attachments.beginArray(); it != attachments.endArray(); ++it)
        {
            const LLSD& objects = (*it)["objects"];
            for (LLSD::array_const_iterator obj = objects.beginArray(); obj != objects.endArray(); ++obj)
            {
                attachment_ids.insert((*obj)["id"].asUUID());
            }
        }
        return true;
    }

    bool agent_settled()
    {
        return !LLApp::isExiting() && gAgent.getTeleportState() == LLAgent::TELEPORT_NONE && isAgentAvatarValid();
    }

    void ghost_temp_attachments_check()
    {
        if (!isAgentAvatarValid())
        {
            return;
        }

        // Let the teleport fully complete and the user stabilise in the new
        // region: the simulator only rezzes the attachments it keeps around this
        // time, so asking any earlier would mistake kept attachments for ghosts.
        llcoro::suspendUntilTimeout(GHOST_TEMP_ATTACHMENT_SETTLE_DELAY);
        F32 wait_so_far = 0.f;
        while (!LLApp::isExiting() && gAgent.getTeleportState() != LLAgent::TELEPORT_NONE &&
               wait_so_far < GHOST_TEMP_ATTACHMENT_MAX_WAIT)
        {
            llcoro::suspendUntilTimeout(1.f);
            wait_so_far += 1.f;
        }
        if (!agent_settled())
        {
            return;
        }

        LLViewerRegion* regionp = gAgent.getRegion();
        if (!regionp)
        {
            return;
        }
        std::string url = regionp->getCapability("AttachmentResources");
        if (url.empty())
        {
            // Without the report we cannot tell a ghost from a live attachment,
            // so leave everything alone rather than risk removing a real one.
            LL_DEBUGS("Avatar") << "Region \"" << regionp->getName()
                                << "\" has no AttachmentResources capability, skipping ghost temp attachment check"
                                << LL_ENDL;
            return;
        }

        std::set<LLUUID> sim_attachment_ids;
        if (!get_region_attachment_ids(url, sim_attachment_ids))
        {
            return;
        }

        // Temp attachments the viewer still wears but the region does not know
        // about are ghost candidates.
        std::set<LLUUID> ghost_candidate_ids;
        for (LLViewerObject* objectp : LLAgentWearables::getTempAttachments())
        {
            if (objectp && !objectp->isDead() && !sim_attachment_ids.count(objectp->getID()))
            {
                ghost_candidate_ids.insert(objectp->getID());
            }
        }
        if (ghost_candidate_ids.empty())
        {
            return;
        }

        // Confirmation pass: an attachment missing from both reports is a ghost,
        // while one that showed up in between merely rezzed late (or got attached
        // by a script while we were checking) and must be kept.
        llcoro::suspendUntilTimeout(GHOST_TEMP_ATTACHMENT_CONFIRM_DELAY);
        if (!agent_settled())
        {
            return;
        }
        regionp = gAgent.getRegion();
        if (!regionp)
        {
            return;
        }
        url = regionp->getCapability("AttachmentResources");
        std::set<LLUUID> confirm_ids;
        if (url.empty() || !get_region_attachment_ids(url, confirm_ids))
        {
            return;
        }
        // The fetch suspended again; the region or the avatar may have gone meanwhile.
        if (!agent_settled() || gAgent.getRegion() != regionp)
        {
            return;
        }

        LLAgentWearables::llvo_vec_t ghosts;
        for (LLViewerObject* objectp : LLAgentWearables::getTempAttachments())
        {
            if (objectp && !objectp->isDead() &&
                ghost_candidate_ids.count(objectp->getID()) && !confirm_ids.count(objectp->getID()))
            {
                ghosts.push_back(objectp);
            }
        }

        for (LLViewerObject* ghostp : ghosts)
        {
            if (!ghostp || ghostp->isDead())
            {
                continue;
            }
            // Remove the whole linkset from the root down, exactly like a detach
            // the simulator initiated: LLViewerObject::markDead() unlinks the root
            // from the avatar, which runs the regular detach bookkeeping (motion
            // cleanup, RLVa, attachment signals, COF updates), and then kills
            // every child of the linkset.
            LLViewerObject* rootp = ghostp->getRootEdit();
            if (!rootp || rootp->isDead())
            {
                continue;
            }
            const LLViewerJointAttachment* attach_pointp = gAgentAvatarp->getTargetAttachmentPoint(ghostp);
            LL_WARNS("Avatar") << "Removing ghost temp attachment " << ghostp->getID() << " on attach point "
                               << (attach_pointp ? attach_pointp->getName() : std::string("unknown"))
                               << ": region \"" << regionp->getName() << "\" does not know about it"
                               << LL_ENDL;
            // <GMDX> A detach the user asked for earlier was ignored by the simulator
            // (it had already dropped the object), so its entry in the doomed temp
            // attachment set would linger forever. Consume it here.
            LLAppearanceMgr::instance().shouldRemoveTempAttachment(ghostp->getID());
            // </GMDX>
            LLSelectMgr::getInstance()->removeObjectFromSelections(rootp->getID());
            gObjectList.killObject(rootp);
        }
    }
}

void LLAppearanceMgr::checkForGhostTempAttachments()
{
    if (sGhostTempAttachmentCheckRunning)
    {
        // A check is already in flight; it waits for the agent to settle and
        // then runs against whatever region the agent ended up in.
        return;
    }
    sGhostTempAttachmentCheckRunning = true;
    LLCoros::instance().launch("LLAppearanceMgr::ghostTempAttachmentsCheckCoro",
                               []()
                               {
                                   struct ClearRunning { ~ClearRunning() { sGhostTempAttachmentCheckRunning = false; } } clear_running;
                                   ghost_temp_attachments_check();
                               });
}
// </SS:Nexii>
