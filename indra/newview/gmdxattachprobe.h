/**
 * @file gmdxattachprobe.h
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

#ifndef GMDX_ATTACHPROBE_H
#define GMDX_ATTACHPROBE_H

class LLVOAvatar;
class LLViewerObject;

// Permanent derender and sound emitter blacklist entries find a worn attachment again after
// a re-rez by wearer + object name, which only an ObjectProperties reply carries; for a worn
// attachment the sim answers only a select + deselect (two reliable messages). Attachments
// arrive on every login, teleport, region crossing and neighbour region, so the asks are
// limited: only while one of the lists still wants that wearer's properties, never twice for
// the same object in a session, and a few per wearer per window, the rest waiting their turn.
namespace GMDXAttachProbe
{
    // LLVOAvatar::addChild, after the derender list has seen the attachment.
    void onAttachmentAdded(LLVOAvatar* wearer, LLViewerObject* attachment);
}

#endif // GMDX_ATTACHPROBE_H
