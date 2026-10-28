/**
 * @file gmdxmapshare.cpp
 * @brief GunMetal-DX: sharing mini-map marks and zones, and pings (see gmdxmapshare.h).
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

// <GMDX> whole file: see gmdxmapshare.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxmapshare.h"

#include "fscombataim.h"
#include "llagent.h"
#include "llagentcamera.h"
#include "llavatarnamecache.h"
#include "llcallbacklist.h"
#include "llcachename.h"
#include "llfloatergroups.h"
#include "llfloaterreg.h"
#include "llfontgl.h"
#include "llgroupcolormap.h"
#include "llgroupmgr.h"
#include "llhudview.h"
#include "llmenugl.h"
#include "llmutelist.h"
#include "llnotificationsutil.h"
#include "llrender.h"
#include "llrender2dutils.h"
#include "llselectmgr.h"
#include "llui.h"
#include "llviewercamera.h"
#include "llviewercontrol.h"
#include "llviewernetwork.h"
#include "llviewerobjectlist.h"
#include "llviewerregion.h"
#include "llviewerwindow.h"
#include "llvoavatar.h"
#include "llworld.h"
#include "llworldmap.h"
#include "llworldmapmessage.h"
#include "llworldmapview.h"
#include "rlvactions.h"
#include "rlvcommon.h"
#include "slbridge.h"

#include <deque>
#include <map>

namespace
{
using Target = GMDXMapShare::Target;
using Item = GMDXShareText::Item;
using Type = GMDXShareText::Type;

constexpr char BRIDGE_USER[] = "gmdx_mapshare";
constexpr size_t MAX_ITEMS = 200;          // received, all senders
constexpr size_t MAX_PER_SENDER = 40;
constexpr size_t MAX_PENDING = 20;
constexpr size_t MAX_PENDING_PER_SENDER = 3;   // (one sender cannot fill the queue or keep the map busy with lookups)
constexpr size_t MAX_RECIPIENTS = 50;      // the helper's own cap too (MAX_TO)
constexpr size_t MAX_BRIDGE_LINE = 980;    // "msay|<seq>|" and the line within the helper's 1000-byte command
constexpr size_t KEYS_PER_COMMAND = 24;    // "mto|<seq>|" and 24 keys with commas: under 1000 bytes
constexpr size_t MAX_QUEUED = 4;           // helper commands already waiting: more, and a send is refused (0.8 s late)
constexpr F64 RESOLVE_SECONDS = 10.0;      // a link to a region not known yet waits this long for the map
constexpr F64 GONE_SECONDS = 3.0;          // a sender out of my region this long: their items go
constexpr F64 TARGET_GONE_SECONDS = 10.0;  // an added (not kept) person out of my region this long: dropped
constexpr F64 MEMBERS_FRESH = 900.0;       // a group's member list is fetched again after this
constexpr F64 MEMBERS_RETRY = 30.0;        // ... or, while it has none, after this
constexpr S32 PING_COLORS[3] = { 2, 0, 3 };   // ping yellow, enemy red, rally green (GMDXMinimapMarks::color)

std::vector<GMDXSharedItem> sItems;

struct Pending { Item text; LLUUID sender; std::string name; F64 until; };
std::vector<Pending> sPending;              // links to regions this viewer does not know yet
std::map<std::string, F64> sAsked;          // region name -> when the map was asked about it

std::vector<Target> sTargets;
std::map<LLUUID, F64> sMembersAsked;        // group -> when its member list was asked for
std::map<LLUUID, F64> sGoneSince;           // senders of what I hold, out of my region since

std::deque<F64> sShareTimes, sPingTimes;
U32 sSendSeq = 0;

// Receiving: a sender's lines closer together than this are dropped; the ping sound at most once a second
constexpr F64 MIN_SENDER_GAP = 0.5;
std::map<LLUUID, F64> sLastFrom;
F64 sLastSound = 0.0;

F64 now() { return LLFrameTimer::getTotalSeconds(); }

void tip(const std::string& text)
{
    LLNotificationsUtil::add("SystemMessageTip", LLSD().with("MESSAGE", text));
}

// Labels for menus: no "[...]" substitution tokens.
std::string menu_label(std::string s)
{
    s = GMDXShareText::clean(s, 64);
    LLStringUtil::replaceChar(s, '[', '(');
    LLStringUtil::replaceChar(s, ']', ')');
    return s;
}

// Rate limit: true (and counted) when the last was at least gap seconds ago and fewer than
// in_30s were in the last 30 seconds.
bool allow(std::deque<F64>& times, F64 gap, size_t in_30s)
{
    const F64 t = now();
    while (!times.empty() && t - times.front() > 30.0) times.pop_front();
    if ((!times.empty() && t - times.back() < gap) || times.size() >= in_30s) return false;
    times.push_back(t);
    return true;
}

// A per-account list of ids (an LLSD array): GMDXMapShareBlocked
bool listed(const char* setting, const LLUUID& id)
{
    const LLSD list = gSavedPerAccountSettings.getLLSD(setting);
    for (const LLSD& v : llsd::inArray(list))
        if (v.asUUID() == id) return true;
    return false;
}

void set_listed(const char* setting, const LLUUID& id, bool on)
{
    const LLSD list = gSavedPerAccountSettings.getLLSD(setting);
    LLSD out = LLSD::emptyArray();
    for (const LLSD& v : llsd::inArray(list))
        if (v.asUUID() != id) out.append(v);
    if (on) out.append(LLSD(id));
    gSavedPerAccountSettings.setLLSD(setting, out);
}

// ---- who is here ------------------------------------------------------------------------------

// The avatars in my region (the region's coarse locations: everyone in it, me left out).
const std::vector<LLUUID>& here_list()
{
    static const std::vector<LLUUID> none;
    const LLViewerRegion* region = gAgent.getRegion();
    return region ? region->mMapAvatarIDs : none;
}

bool here(const LLUUID& id)
{
    const std::vector<LLUUID>& ids = here_list();
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

// A member of the group as far as this viewer knows: its member list once fetched (it alone decides then), else the
// group's tag worn (when the group colours resolve tags) while the list loads.
bool member(const LLUUID& group_id, const LLUUID& avatar_id)
{
    if (group_id.isNull()) return false;   // (no group: an untagged avatar's "active group" is null too)
    LLGroupMgrGroupData* data = LLGroupMgr::getInstance()->getGroupData(group_id);
    // the fetched list decides once complete (an ejected member's attachment can keep the tag)
    if (data && data->isMemberDataComplete()) return data->mMembers.count(avatar_id) > 0;
    return LLGroupColorMap::instance().getActiveGroupID(avatar_id) == group_id;
}

bool members_known(const LLUUID& group_id)
{
    LLGroupMgrGroupData* data = LLGroupMgr::getInstance()->getGroupData(group_id);
    return data && data->isMemberDataComplete();
}

// Keeps the group's member list fetched (one group at a time: the group manager has one request
// in flight at most; a refused one is asked again later).
void want_members(const LLUUID& group_id)
{
    if (group_id.isNull() || !gAgent.isInGroup(group_id)) return;
    F64& asked = sMembersAsked[group_id];
    if (now() - asked < (members_known(group_id) ? MEMBERS_FRESH : MEMBERS_RETRY)) return;
    asked = now();
    LLGroupMgr::getInstance()->sendCapGroupMembersRequest(group_id);
}

// ---- targets ----------------------------------------------------------------------------------

LLUUID my_group()
{
    static LLCachedControl<bool> my(gSavedPerAccountSettings, "GMDXMapShareMyGroup", true);
    return my ? gAgent.getGroupID() : LLUUID::null;
}

// The groups my shares go to: mine (its tag worn) and the ticked ones
std::vector<LLUUID> send_groups()
{
    std::vector<LLUUID> out;
    if (my_group().notNull()) out.push_back(my_group());
    for (const Target& t : sTargets)
        if (t.on && t.kind == Target::GROUP && std::find(out.begin(), out.end(), t.id) == out.end()) out.push_back(t.id);
    return out;
}

// Kept targets: GMDXMapShareTargetGroups / GMDXMapShareTargetPeople, arrays of { id, on }
void save_kept()
{
    LLSD groups = LLSD::emptyArray(), people = LLSD::emptyArray();
    for (const Target& t : sTargets)
    {
        if (!t.kept) continue;
        (t.kind == Target::GROUP ? groups : people).append(LLSD().with("id", t.id).with("on", t.on));
    }
    gSavedPerAccountSettings.setLLSD("GMDXMapShareTargetGroups", groups);
    gSavedPerAccountSettings.setLLSD("GMDXMapShareTargetPeople", people);
}

void load_kept()
{
    sTargets.clear();
    static const std::pair<const char*, Target::Kind> kLists[] = { { "GMDXMapShareTargetGroups", Target::GROUP },
                                                                   { "GMDXMapShareTargetPeople", Target::PERSON } };
    for (const auto& [setting, kind] : kLists)
    {
        for (const LLSD& v : llsd::inArray(gSavedPerAccountSettings.getLLSD(setting)))
        {
            Target t;
            t.kind = kind;
            t.id = v["id"].asUUID();
            t.on = v.has("on") ? v["on"].asBoolean() : true;
            t.kept = true;
            if (t.id.notNull() && t.id != gAgentID) sTargets.push_back(t);
        }
    }
}

Target* find_target(const LLUUID& id)
{
    for (Target& t : sTargets)
        if (t.id == id) return &t;
    return nullptr;
}

// ---- received items ---------------------------------------------------------------------------

void tick(void* = nullptr);

void ensure_idle()
{
    static bool added = false;
    if (!added) gIdleCallbacks.addFunction(tick, nullptr);
    added = true;
}

bool region_origin(const std::string& name, LLVector3d& origin)
{
    for (LLViewerRegion* r : LLWorld::getInstance()->getRegionList())
    {
        if (r && LLStringUtil::compareInsensitive(r->getName(), name) == 0)
        {
            origin = r->getOriginGlobal();
            return true;
        }
    }
    if (LLSimInfo* s = LLWorldMap::getInstance()->simInfoFromName(name))
    {
        origin = s->getGlobalOrigin();
        return true;
    }
    return false;
}

// A received (or my own sent ping) item at its region's origin.
void add_item(const Item& t, const LLUUID& sender, const std::string& name, const LLVector3d& origin)
{
    GMDXSharedItem s;
    s.id.generate();
    s.type = t.type;
    s.sender = sender;
    s.sender_name = GMDXShareText::clean(name, 64);
    s.arrived = now();
    const LLVector3d at = origin + LLVector3d(t.x, t.y, t.z);
    if (t.type == Type::Zone)
    {
        GMDXMapZone& z = s.zone;
        z.id = s.id;
        z.name = t.label;
        z.color = llclamp(t.color, 0, GMDXMinimapMarks::COLORS - 1);
        z.fill = llclamp(t.fill, (S32)GMDXMapZone::FILL_NONE, (S32)GMDXMapZone::FILL_CAUTION);
        z.dotted = t.dotted;
        const LLVector3d c(at.mdV[VX], at.mdV[VY], 0.0);
        if (t.shape == GMDXShareText::Shape::Circle)
        {
            z.kind = GMDXMapZone::CIRCLE;
            z.points = { c, c + LLVector3d(t.r, 0.0, 0.0) };
        }
        else if (t.shape == GMDXShareText::Shape::Poly)
        {
            z.kind = GMDXMapZone::POLYGON;
            for (const auto& [dx, dy] : t.corners) z.points.push_back(c + LLVector3d(dx, dy, 0.0));
        }
        else if (fabs(t.rot) < 0.25)
        {
            z.kind = GMDXMapZone::RECT;
            z.points = { c - LLVector3d(t.w * 0.5, t.h * 0.5, 0.0), c + LLVector3d(t.w * 0.5, t.h * 0.5, 0.0) };
        }
        else   // a turned rectangle: its four corners
        {
            z.kind = GMDXMapZone::POLYGON;
            const F64 a = t.rot * DEG_TO_RAD, ca = cos(a), sa = sin(a);
            static const std::pair<F64, F64> kCorners[4] = { { -1.0, -1.0 }, { 1.0, -1.0 }, { 1.0, 1.0 }, { -1.0, 1.0 } };
            for (const auto& [sx, sy] : kCorners)
            {
                const F64 x = sx * t.w * 0.5, y = sy * t.h * 0.5;
                z.points.push_back(c + LLVector3d(x * ca - y * sa, x * sa + y * ca, 0.0));
            }
        }
    }
    else
    {
        GMDXMapMark& m = s.mark;
        m.id = s.id;
        m.name = t.label;
        m.region = t.region;
        m.pos_global = at;
        m.color = llclamp(t.color, 0, GMDXMinimapMarks::COLORS - 1);
        if (t.type == Type::Ping)
        {
            // on an object or avatar: where this viewer has it now (see follow_anchors), else the sent place
            if (!t.obj.empty() && s.anchor.set(t.obj, false))
            {
                s.on = t.on;
                if (t.has_off) s.anchor_off.set((F32)t.off[0], (F32)t.off[1], (F32)t.off[2]);
                if (const LLViewerObject* o = gObjectList.findObject(s.anchor); o && !o->isDead())
                    m.pos_global = o->getPositionGlobal() + LLVector3d(s.anchor_off * o->getRotationRegion());
            }
            static LLCachedControl<F32> seconds(gSavedSettings, "GMDXPingSeconds", 10.f);
            s.ping = llclamp(t.ping, 0, 2);
            s.map_only = t.map_only;
            m.color = PING_COLORS[s.ping];
            s.expires = s.arrived + llclamp((F32)seconds, 3.f, 60.f);   // (the preference's range)
        }
    }
    // the same again from the sender (a resend, or a newer ping of the same kind): replaced
    sItems.erase(std::remove_if(sItems.begin(), sItems.end(), [&](const GMDXSharedItem& o)
        {
            if (o.sender != sender || o.type != s.type) return false;
            if (s.type == Type::Ping) return o.ping == s.ping;
            if (s.type == Type::Mark) return o.mark.name == s.mark.name && dist_vec(o.mark.pos_global, s.mark.pos_global) < 0.5;
            return o.zone.name == s.zone.name && o.zone.kind == s.zone.kind && o.zone.points == s.zone.points;
        }), sItems.end());
    size_t from_sender = 0;
    for (const GMDXSharedItem& o : sItems) from_sender += o.sender == sender;
    for (auto it = sItems.begin(); it != sItems.end() && from_sender >= MAX_PER_SENDER;)
    {
        if (it->sender == sender) { it = sItems.erase(it); --from_sender; }
        else ++it;
    }
    if (sItems.size() >= MAX_ITEMS) sItems.erase(sItems.begin(), sItems.begin() + (sItems.size() - MAX_ITEMS + 1));
    sItems.push_back(std::move(s));
    ensure_idle();

    static LLCachedControl<bool> sound(gSavedSettings, "GMDXPingSound", true);
    if (t.type == Type::Ping && sender != gAgentID && sound && now() - sLastSound >= 1.0)
    {
        sLastSound = now();
        make_ui_sound("UISndChatPing");
    }
}

// Places a received item now, or once the map knows its region.
void place_received(const Item& t, const LLUUID& sender, const std::string& name)
{
    LLVector3d origin;
    if (region_origin(t.region, origin))
    {
        add_item(t, sender, name, origin);
        return;
    }
    if ((size_t)std::count_if(sPending.begin(), sPending.end(), [&sender](const Pending& p) { return p.sender == sender; })
        >= MAX_PENDING_PER_SENDER)
        return;   // dropped, as an unknown region's line is
    if (sPending.size() >= MAX_PENDING) sPending.erase(sPending.begin());
    sPending.push_back({ t, sender, name, now() + RESOLVE_SECONDS });
    if (sAsked.size() > 64) sAsked.clear();   // (bounded: forgetting only asks the map again)
    F64& asked = sAsked[t.region];
    if (now() - asked > 30.0)   // (the map's name lookup, as a clicked map link asks it)
    {
        asked = now();
        LLWorldMapMessage::getInstance()->sendNamedRegionRequest(t.region);
    }
    ensure_idle();
}

// A sender out of my region for GONE_SECONDS (the region's avatar list can lag an arrival by a
// second, so not at once)
bool sender_gone(const LLUUID& sender, F64 t)
{
    if (sender == gAgentID || here(sender))
    {
        sGoneSince.erase(sender);
        return false;
    }
    const auto [it, first] = sGoneSince.emplace(sender, t);
    return !first && t - it->second >= GONE_SECONDS;
}

// I left the region: nothing shared there is mine to keep showing.
void on_region_changed()
{
    sItems.clear();
    sPending.clear();
    sGoneSince.clear();
}

// Every frame: a ping on an object or avatar is where it is now (vehicles, people running), and
// where it was last seen once it is gone (out of view, derezzed), until the ping is over.
void follow_anchors()
{
    for (GMDXSharedItem& s : sItems)
    {
        if (s.anchor.isNull()) continue;
        if (const LLViewerObject* o = gObjectList.findObject(s.anchor); o && !o->isDead())
            s.mark.pos_global = o->getPositionGlobal() + LLVector3d(s.anchor_off * o->getRotationRegion());
    }
}

// Idle, a few times a second: expired pings, links waiting for their region, senders and added
// people who left my region, the member lists of the groups I send to (and take shares from).
void tick(void*)
{
    follow_anchors();
    static F64 s_last = 0.0;
    const F64 t = now();
    if (t - s_last < 0.25) return;
    s_last = t;
    sItems.erase(std::remove_if(sItems.begin(), sItems.end(), [t](const GMDXSharedItem& s)
                                { return (s.expires > 0.0 && t >= s.expires) || sender_gone(s.sender, t); }),
                 sItems.end());
    for (auto it = sPending.begin(); it != sPending.end();)
    {
        LLVector3d origin;
        if (sender_gone(it->sender, t) || t >= it->until) it = sPending.erase(it);
        else if (region_origin(it->text.region, origin))
        {
            const Pending p = *it;
            it = sPending.erase(it);
            add_item(p.text, p.sender, p.name, origin);
        }
        else ++it;
    }
    if (sGoneSince.size() > 256) sGoneSince.clear();   // (bounded: forgetting only restarts a wait)

    static F64 s_slow = 0.0;
    if (t - s_slow < 1.0 || !gAgent.getRegion()) return;
    s_slow = t;
    // an added person gone from my region for a while is dropped, unless kept
    for (auto it = sTargets.begin(); it != sTargets.end();)
    {
        if (it->kind == Target::PERSON && !here(it->id))
        {
            if (it->gone_since == 0.0) it->gone_since = t;
            if (!it->kept && t - it->gone_since >= TARGET_GONE_SECONDS)
            {
                it = sTargets.erase(it);
                continue;
            }
        }
        else it->gone_since = 0.0;
        ++it;
    }
    for (const LLUUID& g : send_groups()) want_members(g);
}

// ---- sending ----------------------------------------------------------------------------------

// The targets' people in my region now: the ticked people, then my groups' members, at most
// MAX_RECIPIENTS, never me; only_group: just that group's members here. Never anyone else: no
// targets, nobody. loading: a group's member list is not here yet (only tags count).
// ponytail: the first MAX_RECIPIENTS in list order, not the nearest; sort by distance if a region
// ever has more members of my groups than that.
std::vector<LLUUID> recipients(bool& loading, const LLUUID& only_group)
{
    std::vector<LLUUID> out;
    auto add = [&out](const LLUUID& id)
    {
        if (out.size() < MAX_RECIPIENTS && id.notNull() && id != gAgentID && RlvActions::canSendIM(id)
            && std::find(out.begin(), out.end(), id) == out.end())
            out.push_back(id);
    };
    if (only_group.isNull())
        for (const Target& t : sTargets)
            if (t.on && t.kind == Target::PERSON && here(t.id)) add(t.id);
    for (const LLUUID& g : only_group.notNull() ? std::vector<LLUUID>{ only_group } : send_groups())
    {
        if (!gAgent.isInGroup(g)) continue;   // (a kept group I have left)
        want_members(g);
        loading |= !members_known(g);
        for (const LLUUID& id : here_list())
            if (member(g, id)) add(id);
    }
    return out;
}

// The line through my helper to each recipient (see slipstream_helper.lsl: mto, msay).
bool bridge_send(const std::vector<LLUUID>& to, const std::string& line)
{
    SLBridge& bridge = SLBridge::instance();
    const std::string seq = llformat("%u", ++sSendSeq % 1000000000u);
    bool ok = true;
    for (size_t i = 0; i < to.size(); i += KEYS_PER_COMMAND)
    {
        std::string keys;
        for (size_t k = i; k < llmin(to.size(), i + KEYS_PER_COMMAND); ++k) keys += (keys.empty() ? "" : ",") + to[k].asString();
        ok &= bridge.sendCommand("mto|" + seq + "|" + keys);
    }
    // (an msay without its list says nothing: the helper matches the seq)
    return ok && bridge.sendCommand("msay|" + seq + "|" + line);
}

// Can anything go out now? The helper must be worn and answering.
bool can_send()
{
    if (!gSavedSettings.getBOOL("GMDXMapShareSend"))
    {
        tip("Sending map shares and pings is off: Preferences > Combat > Map Share.");
        return false;
    }
    if (!SLBridge::instanceExists() || !SLBridge::instance().isReady())
    {
        const std::string why = SLBridge::instanceExists() ? SLBridge::instance().getReason() : std::string();
        tip("Map shares and pings go through the #GunMetal-DX Helper, which is not ready"
            + (why.empty() ? std::string(" yet: try again in a moment.") : ": " + why + "."));
        return false;
    }
    return true;
}

void deliver(const Item& t, bool is_ping, bool group_only, std::function<void()> sent = nullptr);

// Nobody chosen and no group tag worn: pick a group (it is added for this session), then send.
void pick_group_then(std::function<void()> then)
{
    static LLHandle<LLFloater> s_connected;
    static std::function<void()> s_then;
    // its own picker (the key only tells instances apart), so other features' picks never land here
    static const LLUUID kPickerKey("6d617073-6861-7265-2d67-726f75707069");
    LLFloaterGroupPicker* picker = LLFloaterReg::showTypedInstance<LLFloaterGroupPicker>("group_picker", LLSD(kPickerKey));
    if (!picker) return;
    s_then = std::move(then);
    if (picker->getHandle() != s_connected)   // (one slot per picker instance)
    {
        s_connected = picker->getHandle();
        picker->setSelectGroupCallback([](LLUUID id)
            {
                std::function<void()> next;
                std::swap(next, s_then);
                if (id.isNull() || !gAgent.isInGroup(id)) return;
                GMDXMapShare::addTarget(Target::GROUP, id);
                if (next) next();
            });
    }
}

// group_only (Share to Group): only my group's members here, the one whose tag I wear now.
void deliver(const Item& t, bool is_ping, bool group_only, std::function<void()> sent)
{
    const std::string line = GMDXShareText::compose(t, LLGridManager::getInstance()->getSLURLBase());
    if (line.empty() || line.size() > MAX_BRIDGE_LINE)
    {
        tip("That is too big for one chat line: a zone with fewer corners (or a shorter name) fits.");
        return;
    }
    if (!can_send()) return;
    const LLUUID only_group = group_only ? gAgent.getGroupID() : LLUUID::null;
    if (group_only && only_group.isNull())
    {
        tip("You wear no group tag: put on the tag of the group to share with.");
        return;
    }
    if (!group_only && send_groups().empty() && std::none_of(sTargets.begin(), sTargets.end(), [](const Target& x) { return x.on; }))
    {
        if (gSavedPerAccountSettings.getBOOL("GMDXMapShareMyGroup") && gAgent.getGroupID().isNull())
        {
            tip("You wear no group tag: choose the group to send to (it is added to Send To for this session).");
            pick_group_then([t, is_ping, sent]() { deliver(t, is_ping, false, sent); });
        }
        else tip("Choose who gets your shares and pings: mini-map menu > Ping > Send To.");
        return;
    }
    bool loading = false;
    const std::vector<LLUUID> to = recipients(loading, only_group);
    if (to.empty())
    {
        tip(loading ? "Your group's member list is still loading: try again in a moment."
                    : group_only ? "Nobody else from your group is in this region. Shares only reach people here."
                                 : "Nobody you send to is in this region. Shares and pings only reach people here.");
        return;
    }
    // the helper's queue is shared with pose data and paced (5 a second): a backlog would make a
    // ping late or drop part of it
    if (SLBridge::instance().getQueuedCommands() > MAX_QUEUED)
    {
        tip("Your helper is busy: try again in a moment.");
        return;
    }
    if (is_ping ? !allow(sPingTimes, 2.0, 5) : !allow(sShareTimes, 1.0, 10))
    {
        static F64 s_tipped = 0.0;   // (held down, the key would stack tips)
        if (now() - s_tipped >= 3.0)
        {
            s_tipped = now();
            tip(is_ping ? "Slow down: one ping every 2 seconds, 5 in 30 seconds." : "Slow down: one share a second, 10 in 30 seconds.");
        }
        return;
    }
    if (!bridge_send(to, line))
    {
        tip("Your helper did not take that: try again in a moment.");
        return;
    }
    if (!is_ping) tip(llformat("Sharing with %d %s in this region.", (S32)to.size(), to.size() == 1 ? "person" : "people"));
    if (sent) sent();
}

// The item's region and whole-metre position there; at (optional): that point, global.
bool place(const LLVector3d& pos_global, Item& t, LLVector3d* at = nullptr)
{
    if (!RlvActions::canShowLocation())
    {
        tip("Your location is hidden (RLV): map items cannot be shared.");
        return false;
    }
    LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(pos_global);
    if (!region)
    {
        tip("That place is not in a region you can see.");
        return false;
    }
    const LLVector3 p = region->getPosRegionFromGlobal(pos_global);
    t.region = region->getName();
    t.x = llmax(0, ll_round(p.mV[VX]));
    t.y = llmax(0, ll_round(p.mV[VY]));
    t.z = llmax(0, ll_round(p.mV[VZ]));
    if (at) *at = region->getOriginGlobal() + LLVector3d(t.x, t.y, t.z);
    return true;
}

// ---- receiving --------------------------------------------------------------------------------

// left as chat text; left as chat text and asked about (a stranger's share); taken out; taken out and drawn
enum class Verdict { Text, Ask, Hide, Draw };

// Who may draw on my map: the people in my list (Send To: ticked or not, kept or for this session)
// and the members of the groups I send to (my group while GMDXMapShareMyGroup, the ticked ones).
// Nobody else, friends neither: their lines stay chat text and ask.
bool trusted(const LLUUID& from_id)
{
    if (const Target* t = find_target(from_id); t && t->kind == Target::PERSON) return true;
    for (const LLUUID& g : send_groups())
        if (gAgent.isInGroup(g) && member(g, from_id)) return true;   // (not a kept group I have left)
    return false;
}

// A share line from someone's helper.
Verdict decide(const LLUUID& from_id)
{
    if (from_id == gAgentID) return Verdict::Hide;   // (my own helper never says one to me)
    if (LLMuteList::getInstance()->isMuted(from_id)) return Verdict::Text;   // (as any line of theirs)
    // [RLVa] @recvim: dropped, as their IM would be
    if (!RlvActions::canReceiveIM(from_id)) return Verdict::Hide;
    // blocked (over my list and groups): stays the readable text it is (any viewer could have sent it)
    if (listed("GMDXMapShareBlocked", from_id)) return Verdict::Text;
    return trusted(from_id) ? Verdict::Draw : Verdict::Ask;
}

// Always (a stranger's ask): in my list, kept, but not sent to until ticked in Send To (taking their
// shares sends them nothing of mine).
void keep_listed(const LLUUID& id)
{
    if (id.isNull() || id == gAgentID) return;
    Target* t = find_target(id);
    if (!t)
    {
        sTargets.push_back(Target{ Target::PERSON, id, false });
        t = &sTargets.back();
    }
    t->kept = true;
    save_kept();
}

// A stranger's mark or zone (its line hidden, the popup stands for it): "You've received map information from
// <name>": Accept (draw this one) / Always (add them to my list, kept, then draw it) / Deny (nothing,
// and not asked about them again for ASK_SECONDS) / Mute (GMDXMapShareBlocked, not the mute list).
// One ask per sender at a time, one every 2 s and 3 in 30 s in all; a line past that is dropped.
// Pings never ask: a stranger's ping is dropped (consumeObjectChat).
constexpr F64 ASK_SECONDS = 120.0;   // (the notification's duration: then it is gone unanswered)
std::map<LLUUID, F64> sAsking;       // sender -> when asked
std::deque<F64> sAskTimes;

void ask(const LLUUID& from_id, const std::string& from, const std::string& text, const Item& t)
{
    const auto it = sAsking.find(from_id);
    if (it != sAsking.end() && now() - it->second < ASK_SECONDS) return;
    if (!allow(sAskTimes, 2.0, 3)) return;
    if (sAsking.size() > 64) sAsking.clear();   // (bounded: forgetting only lets them be asked about again)
    sAsking[from_id] = now();
    // the name from the name service, never the object's (anyone can call an object "Zone from
    // governor.linden"): asked for first when it is not known yet
    LLAvatarName known;
    if (!LLAvatarNameCache::get(from_id, &known))
    {
        const F64 counted = sAskTimes.back();   // (allow() just counted this ask)
        LLAvatarNameCache::get(from_id, [from, text, t, counted](const LLUUID& id, const LLAvatarName&)
            {
                sAsking.erase(id);
                const auto c = std::find(sAskTimes.begin(), sAskTimes.end(), counted);
                if (c != sAskTimes.end()) sAskTimes.erase(c);
                // still a stranger here (not muted, blocked or listed meanwhile)
                if (here(id) && decide(id) == Verdict::Ask) ask(id, from, text, t);
            });
        return;
    }
    GMDXSharedItem who;
    who.sender = from_id;
    who.sender_name = GMDXShareText::clean(from, 64);
    LLSD args;
    args["NAME"] = menu_label(GMDXMapShare::senderName(who));   // (RLV-aware; no "[...]")
    args["WHAT"] = (t.type == Type::Zone ? "a zone \"" : "a mark \"") + menu_label(t.label) + "\"";
    args["REGION"] = menu_label(t.region);
    const LLSD payload = LLSD().with("from_id", from_id).with("from", from).with("text", text);
    LLNotificationsUtil::add("GMDXMapShareAsk", args, payload, [](const LLSD& notification, const LLSD& response)
        {
            const LLSD& p = notification["payload"];
            const LLUUID sender = p["from_id"].asUUID();
            const S32 option = LLNotificationsUtil::getSelectedOption(notification, response);
            // Deny: not asked about them again for a while (their lines stay text)
            if (option == 2) sAsking[sender] = now();
            else sAsking.erase(sender);
            if (option == 3) GMDXMapShare::block(sender);
            if (option != 0 && option != 1) return;
            if (option == 1) keep_listed(sender);
            if (listed("GMDXMapShareBlocked", sender) || LLMuteList::getInstance()->isMuted(sender)) return;
            if (!here(sender))
            {
                tip("They have left this region: what they shared is gone.");
                return;
            }
            Item t;
            if (GMDXShareText::parse(p["text"].asString(), LLGridManager::getInstance()->getSLURLBase(), t))
                place_received(t, sender, p["from"].asString());
        });
}

// ---- menus ------------------------------------------------------------------------------------

void add_call(LLMenuGL* sub, const std::string& label, bool enabled, std::function<void()> act)
{
    LLMenuItemCallGL::Params p;
    p.name = llformat("gmdx_share_%d", sub->getItemCount());
    p.label = label;
    p.enabled = enabled;
    p.on_click.function([act](LLUICtrl*, const LLSD&) { if (act) act(); });
    sub->addChild(LLUICtrlFactory::create<LLMenuItemCallGL>(p));
}

void add_check(LLMenuGL* sub, const std::string& label, std::function<bool()> checked, std::function<void()> act)
{
    LLMenuItemCheckGL::Params p;
    p.name = llformat("gmdx_share_%d", sub->getItemCount());
    p.label = label;
    p.on_click.function([act](LLUICtrl*, const LLSD&) { if (act) act(); });
    p.on_check.function([checked](LLUICtrl*, const LLSD&) { return checked(); });
    sub->addChild(LLUICtrlFactory::create<LLMenuItemCheckGL>(p));
}

LLMenuGL* add_branch(LLMenuGL* parent, const std::string& label)
{
    LLMenuGL::Params p;
    p.name = llformat("gmdx_share_menu_%d", parent->getItemCount());
    p.label = label;
    p.can_tear_off = false;
    LLMenuGL* sub = LLUICtrlFactory::create<LLMenuGL>(p);
    parent->addChild(sub);
    return sub;
}

// Send To: my group, the added groups and people (each: ticked, Keep, Remove), Add Group...,
// Add Person on Sim.
void fill_send_to(LLMenuGL* sub)
{
    sub->empty();
    LLGroupData data;
    const LLUUID active = gAgent.getGroupID();
    add_check(sub, active.notNull() && gAgent.getGroupData(active, data) ? "My Group (" + menu_label(data.mName) + ")" : std::string("My Group (no tag worn)"),
              []() { return gSavedPerAccountSettings.getBOOL("GMDXMapShareMyGroup"); },
              []() { gSavedPerAccountSettings.setBOOL("GMDXMapShareMyGroup", !gSavedPerAccountSettings.getBOOL("GMDXMapShareMyGroup")); });
    if (!sTargets.empty()) sub->addSeparator();
    for (const Target& t : sTargets)
    {
        std::string label = menu_label(GMDXMapShare::targetName(t));
        if (t.kind == Target::GROUP) label += gAgent.isInGroup(t.id) ? " (group)" : " (group, left)";
        else if (!here(t.id)) label += " (not here)";
        if (!t.on) label += ", off";
        if (t.kept) label += ", kept";
        LLMenuGL* branch = add_branch(sub, label);
        const LLUUID id = t.id;
        add_check(branch, "Send to Them", [id]() { const Target* x = find_target(id); return x && x->on; },
                  [id]() { if (const Target* x = find_target(id)) GMDXMapShare::setTargetOn(id, !x->on); });
        add_check(branch, "Keep (Always)", [id]() { const Target* x = find_target(id); return x && x->kept; },
                  [id]() { if (const Target* x = find_target(id)) GMDXMapShare::setTargetKept(id, !x->kept); });
        add_call(branch, "Remove", true, [id]() { GMDXMapShare::removeTarget(id); });
    }
    sub->addSeparator();
    add_call(sub, "Add Group...", true, []() { pick_group_then(nullptr); });
    LLMenuGL* people = add_branch(sub, "Add Person on Sim");
    std::vector<std::pair<std::string, LLUUID>> names;
    for (const LLUUID& id : here_list())
    {
        if (id == gAgentID || find_target(id)) continue;
        LLAvatarName name;
        const bool known = LLAvatarNameCache::get(id, &name);   // (asks for it when not known)
        Target t{ Target::PERSON, id };
        names.emplace_back(known ? GMDXMapShare::targetName(t) : std::string("(loading name)"), id);
    }
    std::sort(names.begin(), names.end(), [](const auto& a, const auto& b) { return LLStringUtil::compareInsensitive(a.first, b.first) < 0; });
    constexpr size_t MAX_LISTED = 40;
    for (size_t i = 0; i < names.size() && i < MAX_LISTED; ++i)
    {
        const LLUUID id = names[i].second;
        add_call(people, menu_label(names[i].first), true, [id]() { GMDXMapShare::addTarget(Target::PERSON, id); });
    }
    if (names.empty()) add_call(people, "(nobody else in this region)", false, nullptr);
    else if (names.size() > MAX_LISTED) add_call(people, llformat("(%d more not listed)", (S32)(names.size() - MAX_LISTED)), false, nullptr);
}
} // namespace

// ---- setup and targets ------------------------------------------------------------------------

void GMDXMapShare::init()
{
    static bool initialized = false;
    if (initialized) return;
    initialized = true;
    load_kept();
    ensure_idle();
    gAgent.addRegionChangedCallback([]() { on_region_changed(); });
    auto use_bridge = []()
    {
        if (gSavedSettings.getBOOL("GMDXMapShareSend")) SLBridge::instance().addUser(BRIDGE_USER);
        else SLBridge::instance().removeUser(BRIDGE_USER);
    };
    gSavedSettings.getControl("GMDXMapShareSend")->getSignal()->connect([use_bridge](LLControlVariable*, const LLSD&, const LLSD&) { use_bridge(); });
    use_bridge();
    // the helper ran short of script memory or linkset data ("script|<free>" / "lsd|<free>"): it
    // stops map shares (and says so once) until it has room
    SLBridge::instance().registerHandler("mem", [](const std::string& what)
        {
            LL_WARNS("GMDXMapShare") << "Helper low on memory (" << what << " bytes free): map shares paused" << LL_ENDL;
            tip("Your helper is low on memory: map shares and pings are paused until it has room again"
                " (take the #GunMetal-DX Helper off and on if this stays).");
        });
}

const std::vector<Target>& GMDXMapShare::targets()
{
    return sTargets;
}

void GMDXMapShare::addTarget(Target::Kind kind, const LLUUID& id)
{
    if (id.isNull() || id == gAgentID) return;
    if (Target* t = find_target(id))
    {
        t->on = true;
        if (t->kept) save_kept();
        return;
    }
    Target t;
    t.kind = kind;
    t.id = id;
    sTargets.push_back(t);
    if (kind == Target::GROUP) want_members(id);
}

void GMDXMapShare::removeTarget(LLUUID id)
{
    const bool kept = find_target(id) && find_target(id)->kept;
    sTargets.erase(std::remove_if(sTargets.begin(), sTargets.end(), [&id](const Target& t) { return t.id == id; }), sTargets.end());
    if (kept) save_kept();
}

void GMDXMapShare::setTargetOn(const LLUUID& id, bool on)
{
    if (Target* t = find_target(id))
    {
        t->on = on;
        if (t->kept) save_kept();
    }
}

void GMDXMapShare::setTargetKept(const LLUUID& id, bool kept)
{
    if (Target* t = find_target(id))
    {
        t->kept = kept;
        save_kept();
    }
}

std::string GMDXMapShare::targetName(const Target& t)
{
    if (t.kind == Target::GROUP)
    {
        LLGroupData data;
        std::string name;
        if (gAgent.getGroupData(t.id, data)) return data.mName;
        return gCacheName && gCacheName->getGroupName(t.id, name) ? name : std::string("a group");   // (a group I left)
    }
    LLAvatarName name;
    if (!LLAvatarNameCache::get(t.id, &name)) return "someone";
    return RlvActions::canShowName(RlvActions::SNC_DEFAULT, t.id) ? name.getCompleteName() : RlvStrings::getAnonym(name);
}

// ---- sending ----------------------------------------------------------------------------------

void GMDXMapShare::shareMark(const LLUUID& mark_id, bool group_only)
{
    const GMDXMapMark* m = GMDXMinimapMarks::find(mark_id);
    if (!m) return;
    Item t;
    t.type = Type::Mark;
    t.label = m->name;
    t.color = m->color;
    if (place(m->pos_global, t)) deliver(t, false, group_only);
}

void GMDXMapShare::shareZone(const LLUUID& zone_id, bool group_only)
{
    const GMDXMapZone* z = GMDXMinimapMarks::findZone(zone_id);
    if (!z || z->points.size() < 2) return;
    Item t;
    t.type = Type::Zone;
    t.label = z->name;
    t.color = z->color;
    t.fill = z->fill;
    t.dotted = z->dotted;
    const std::vector<LLVector3d>& p = z->points;
    LLVector3d centre;
    if (z->kind == GMDXMapZone::RECT)
    {
        t.shape = GMDXShareText::Shape::Rect;
        centre = (p[0] + p[1]) * 0.5;
        t.w = fabs(p[1].mdV[VX] - p[0].mdV[VX]);
        t.h = fabs(p[1].mdV[VY] - p[0].mdV[VY]);
        if (t.w < 0.5 || t.h < 0.5) { tip("That zone is too small to share."); return; }
    }
    else if (z->kind == GMDXMapZone::CIRCLE)
    {
        t.shape = GMDXShareText::Shape::Circle;
        centre = p[0];
        t.r = sqrt((p[1].mdV[VX] - p[0].mdV[VX]) * (p[1].mdV[VX] - p[0].mdV[VX]) + (p[1].mdV[VY] - p[0].mdV[VY]) * (p[1].mdV[VY] - p[0].mdV[VY]));
        if (t.r < 0.5) { tip("That zone is too small to share."); return; }
    }
    else
    {
        t.shape = GMDXShareText::Shape::Poly;
        LLVector3d lo = p[0], hi = p[0];
        for (const LLVector3d& q : p)
        {
            lo.set(llmin(lo.mdV[VX], q.mdV[VX]), llmin(lo.mdV[VY], q.mdV[VY]), 0.0);
            hi.set(llmax(hi.mdV[VX], q.mdV[VX]), llmax(hi.mdV[VY], q.mdV[VY]), 0.0);
        }
        centre = (lo + hi) * 0.5;
    }
    LLVector3d at;
    if (!place(GMDXMinimapMarks::onGround(LLVector3d(centre.mdV[VX], centre.mdV[VY], 0.0)), t, &at)) return;
    if (t.shape == GMDXShareText::Shape::Poly)
    {
        // the corners from the link's point (whole metres), as the receiver places them
        if (p.size() > GMDXShareText::MAX_CORNERS)
        {
            tip(llformat("That zone has too many corners to share (%d at most).", (S32)GMDXShareText::MAX_CORNERS));
            return;
        }
        for (const LLVector3d& q : p) t.corners.emplace_back(q.mdV[VX] - at.mdV[VX], q.mdV[VY] - at.mdV[VY]);
    }
    deliver(t, false, group_only);
}

namespace
{
// What a ping that hit obj is on: an avatar (for any of its attachments too), else the linkset's
// root; nothing for HUDs and animesh's own avatar (its id is this viewer's alone).
LLViewerObject* anchor_of(LLViewerObject* obj)
{
    if (!obj || obj->isDead() || obj->isHUDAttachment() || (obj->getRootEdit() && obj->getRootEdit()->isHUDAttachment())) return nullptr;
    if (!obj->isAvatar())
    {
        LLVOAvatar* wearer = obj->getAvatarAncestor();   // (an attachment, any prim of it)
        obj = wearer ? static_cast<LLViewerObject*>(wearer) : obj->getRootEdit();
    }
    if (!obj || obj->isDead()) return nullptr;
    if (const LLVOAvatar* av = obj->asAvatar(); av && av->isControlAvatar()) return nullptr;
    return obj;
}

// Its name for the line, as I may see it (RLVa): an avatar's display name, an object's name when
// this viewer has it (the hover's), else "" (the line then just says what kind of ping).
std::string anchor_name(LLViewerObject* obj)
{
    std::string name;
    if (obj->isAvatar())
    {
        LLAvatarName av;
        if (LLAvatarNameCache::get(obj->getID(), &av))
            name = RlvActions::canShowName(RlvActions::SNC_DEFAULT, obj->getID()) ? av.getDisplayName() : RlvStrings::getAnonym(av);
        return name;
    }
    LLSelectNode* node = LLSelectMgr::getInstance()->getHoverNode();
    if (node && node->mValid && node->getObject() && node->getObject()->getRootEdit() == obj) name = node->mName;
    if (!RlvActions::canShowName(RlvActions::SNC_DEFAULT)) RlvUtil::filterNames(name);
    return name;
}

// map_only: placed on a map, so shown on maps only (here and for the receivers)
void ping_at(const LLVector3d& pos_global, S32 kind, LLViewerObject* on, bool map_only)
{
    Item t;
    t.type = Type::Ping;
    t.ping = llclamp(kind, 0, 2);
    t.label = GMDXShareText::pingLabel(t.ping);   // (my own copy's name: the line's is made from the kind)
    t.map_only = map_only;
    // the picked point on it, in its own frame: receivers follow that spot, not the root's origin
    // (or an avatar's middle). Past 64 m from its origin the thing is scenery (a region-wide floor
    // or build mesh, whose middle can be far off): then the ping is just the picked place
    const LLVector3 off = on ? LLVector3(pos_global - on->getPositionGlobal()) * ~on->getRotationRegion() : LLVector3::zero;
    if (on && off.lengthSquared() < 64.f * 64.f)
    {
        t.obj = on->getID().asString();
        t.on = anchor_name(on);
        if (off.lengthSquared() > 0.01f)   // rounded as sent, so my own copy sits where theirs do
        {
            t.has_off = true;
            for (int i = 0; i < 3; ++i) t.off[i] = ll_round(off.mV[i] * 10.f) / 10.0;
        }
    }
    LLVector3d at;
    if (!place(pos_global, t, &at)) return;
    const LLVector3d origin = at - LLVector3d(t.x, t.y, t.z);
    // mine, on my map and over the world too, once it went out
    deliver(t, true, false, [t, origin]() { add_item(t, gAgentID, std::string(), origin); });
}
} // namespace

void GMDXMapShare::ping(const LLVector3d& pos_global, S32 kind)
{
    ping_at(pos_global, kind, nullptr, true);
}

bool GMDXMapShare::pingTarget(PingTarget& target)
{
    LLVector3d pos;
    LLViewerObject* on = nullptr;
    if (gAgentCamera.cameraMouselook())   // (OTS too) the crosshair: the aim's own point under it
    {
        const LLViewerCamera* camera = LLViewerCamera::getInstance();
        const LLVector3 hit = FSCombatAim::getOTSConvergenceTarget(camera->getOrigin(), camera->getAtAxis());
        if (dist_vec(hit, camera->getOrigin()) > 500.f)
        {
            tip("Nothing under the crosshair to ping.");
            return false;
        }
        pos = gAgent.getPosGlobalFromAgent(hit);
        // what is at that point on screen, when the pick there lands on it (not something in front):
        // then the ping is on it at the pick's own surface point, which the aim's point may miss by a little
        LLCoordGL screen;
        if (camera->projectPosAgentToScreen(hit, screen, false))
        {
            const LLPickInfo pick = gViewerWindow->pickImmediate(screen.mX, screen.mY, false, true);
            if (pick.isValid() && dist_vec(pick.mPosGlobal, pos) < 1.5 && (on = anchor_of(pick.getObject())))
                pos = pick.mPosGlobal;
        }
    }
    else
    {
        // a pick where the pointer is now (the hover pick can be frames old), over the world only
        const S32 x = gViewerWindow->getCurrentMouseX(), y = gViewerWindow->getCurrentMouseY();
        if (!gViewerWindow->getPointerOverWorld() || !gViewerWindow->getWorldViewRectScaled().pointInRect(x, y))
        {
            tip("Point at the place in the world to ping (the pointer is over a window).");
            return false;
        }
        const LLPickInfo pick = gViewerWindow->pickImmediate(x, y, false, true);   // (rigged: mesh bodies are people)
        if (!pick.isValid() || pick.mPosGlobal.isExactlyZero())
        {
            tip("Point at the place to ping.");
            return false;
        }
        pos = pick.mPosGlobal;
        on = anchor_of(pick.getObject());
    }
    target.pos = pos;
    target.on = on ? on->getID() : LLUUID::null;
    target.off = on ? LLVector3(pos - on->getPositionGlobal()) * ~on->getRotationRegion() : LLVector3::zero;
    return true;
}

void GMDXMapShare::pingTo(const PingTarget& target, S32 kind)
{
    // on something that moved since (the wheel was open a moment): the same spot on it now
    LLViewerObject* on = target.on.notNull() ? gObjectList.findObject(target.on) : nullptr;
    if (on && on->isDead()) on = nullptr;
    const LLVector3d pos = on ? on->getPositionGlobal() + LLVector3d(target.off * on->getRotationRegion()) : target.pos;
    ping_at(pos, kind, on, false);
}

void GMDXMapShare::pingAtCursor(S32 kind)
{
    PingTarget target;
    if (pingTarget(target)) pingTo(target, kind);
}

LLColor4 GMDXMapShare::pingColor(S32 kind)
{
    return GMDXMinimapMarks::color(PING_COLORS[llclamp(kind, 0, 2)]);
}

// ---- receiving --------------------------------------------------------------------------------

bool GMDXMapShare::consumeObjectChat(const LLUUID& object_id, const LLUUID& owner_id, const std::string& object_name,
                                     const std::string& text)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "GMDXMapShareShow", true);
    static LLCachedControl<bool> as_text(gSavedSettings, "GMDXMapShareShowAsText", false);
    if (!enabled || text.empty() || text[0] != '[' || owner_id.isNull() || owner_id == object_id) return false;
    // named for a share by its helper ("Ping from <username>", slipstream_helper.lsl)
    static const std::pair<const char*, Type> kNames[] = { { "Ping from ", Type::Ping }, { "Mark from ", Type::Mark }, { "Zone from ", Type::Zone } };
    const auto named = std::find_if(std::begin(kNames), std::end(kNames), [&object_name](const auto& n) { return object_name.rfind(n.first, 0) == 0; });
    if (named == std::end(kNames)) return false;
    // its owner is an avatar in my region (an attachment is where its wearer is), and an object I
    // can see is worn by them (another's helper is a HUD, never sent to me: then the region's list,
    // which an object of theirs out of view also passes; the owner, from the sim, is right either way)
    if (const LLViewerObject* obj = gObjectList.findObject(object_id))
    {
        const LLVOAvatar* wearer = obj->isAttachment() ? obj->getAvatar() : nullptr;
        if (!wearer || wearer->getID() != owner_id) return false;
    }
    else if (!here(owner_id)) return false;
    Item t;
    if (!GMDXShareText::parse(text, LLGridManager::getInstance()->getSLURLBase(), t) || t.type != named->second) return false;

    // (the object's name is the sender's own word: never shown as who they are, see ask / senderName)
    const std::string from;
    Verdict verdict = decide(owner_id);
    if (verdict == Verdict::Ask && t.type == Type::Ping) verdict = Verdict::Text;   // (pings never ask: not drawn, no sound)
    if (verdict == Verdict::Ask) ask(owner_id, from, text, t);
    // a share line from a helper never shows as chat on GMDX (the popup or the map stands for it;
    // a stranger's ping and a blocked sender's lines just go): only a muted sender's is left to the
    // mute list, which hides it anyway, and "show as text" keeps every line
    if (verdict == Verdict::Text && LLMuteList::getInstance()->isMuted(owner_id)) return false;
    if (verdict == Verdict::Draw)
    {
        // a sender faster than MIN_SENDER_GAP loses the extra lines
        if (sLastFrom.size() > 256) sLastFrom.clear();
        F64& last = sLastFrom[owner_id];
        if (now() - last >= MIN_SENDER_GAP)
        {
            last = now();
            place_received(t, owner_id, from);
        }
    }
    return !as_text;
}

const std::vector<GMDXSharedItem>& GMDXMapShare::items()
{
    return sItems;   // (expired pings go in the idle tick; until then they draw fully faded)
}

const GMDXSharedItem* GMDXMapShare::find(const LLUUID& id)
{
    for (const GMDXSharedItem& s : sItems)
        if (s.id == id) return &s;
    return nullptr;
}

std::string GMDXMapShare::senderName(const GMDXSharedItem& item)
{
    if (item.sender == gAgentID) return "You";
    LLAvatarName name;
    if (LLAvatarNameCache::get(item.sender, &name))
    {
        return GMDXShareText::clean(RlvActions::canShowName(RlvActions::SNC_DEFAULT, item.sender) ? name.getDisplayName() : RlvStrings::getAnonym(name), 32);
    }
    if (!RlvActions::canShowName(RlvActions::SNC_DEFAULT, item.sender)) return RlvStrings::getAnonym(item.sender_name);
    return item.sender_name.empty() ? std::string("someone") : GMDXShareText::clean(item.sender_name, 32);
}

std::string GMDXMapShare::anchorName(const GMDXSharedItem& item)
{
    if (item.anchor.isNull()) return std::string();
    // an avatar I know: their name as I may see it (RLVa); else the name as sent, filtered as any name
    // in text is under @shownames
    const LLViewerObject* o = gObjectList.findObject(item.anchor);
    LLAvatarName av;
    if (o && o->isAvatar() && LLAvatarNameCache::get(item.anchor, &av))
    {
        return GMDXShareText::clean(RlvActions::canShowName(RlvActions::SNC_DEFAULT, item.anchor) ? av.getDisplayName() : RlvStrings::getAnonym(av), 32);
    }
    std::string name = GMDXShareText::clean(item.on, 32);
    if (!RlvActions::canShowName(RlvActions::SNC_DEFAULT)) RlvUtil::filterNames(name);
    return name;
}

LLColor4 GMDXMapShare::senderColor(const GMDXSharedItem& item)
{
    return LLGroupColorMap::instance().tint(item.sender, LLColor4(0.86f, 0.88f, 0.9f, 1.f));
}

void GMDXMapShare::keep(const LLUUID& id)
{
    const GMDXSharedItem* s = find(id);
    if (!s) return;
    if (s->type == Type::Zone)
    {
        GMDXMapZone z = s->zone;
        z.id.setNull();
        GMDXMinimapMarks::addZone(z);
    }
    else
    {
        GMDXMinimapMarks::add(s->mark.pos_global, s->mark.name.empty() ? std::string("Mark") : s->mark.name, s->mark.color);
    }
}

void GMDXMapShare::remove(LLUUID id)
{
    sItems.erase(std::remove_if(sItems.begin(), sItems.end(), [&id](const GMDXSharedItem& s) { return s.id == id; }), sItems.end());
}

void GMDXMapShare::removeFrom(LLUUID sender)
{
    sItems.erase(std::remove_if(sItems.begin(), sItems.end(), [&sender](const GMDXSharedItem& s) { return s.sender == sender; }), sItems.end());
}

void GMDXMapShare::block(LLUUID sender)
{
    if (sender.isNull() || sender == gAgentID) return;
    GMDXSharedItem who;
    who.sender = sender;
    const std::string name = senderName(who);
    set_listed("GMDXMapShareBlocked", sender, true);
    removeFrom(sender);
    sPending.erase(std::remove_if(sPending.begin(), sPending.end(), [&sender](const Pending& p) { return p.sender == sender; }), sPending.end());
    tip(name + "'s map shares are blocked: none are drawn now. Undo: mini-map > Show > Unblock All Map Shares.");
}

void GMDXMapShare::unblockAll()
{
    gSavedPerAccountSettings.setLLSD("GMDXMapShareBlocked", LLSD::emptyArray());
}

void GMDXMapShare::clear()
{
    sItems.clear();
    sPending.clear();
}

// ---- menus ------------------------------------------------------------------------------------

void GMDXMapShare::fillShareMenus(LLMenuGL* menu)
{
    for (const char* name : { "Share Send To", "Ping Send To" })
        if (LLMenuGL* sub = menu->findChildMenuByName(name, true)) fill_send_to(sub);
    // Share to Group: only the group whose tag I wear (none worn: off, and says so)
    if (LLMenuGL* share = menu->findChildMenuByName("Share With", true))
    {
        LLGroupData data;
        const LLUUID active = gAgent.getGroupID();
        const bool tag = active.notNull() && gAgent.getGroupData(active, data);
        share->setItemLabel("Share Group", tag ? "Share to " + menu_label(data.mName) : std::string("Share to Group (no group tag worn)"));
        share->setItemEnabled("Share Group", tag);
        if (LLMenuItemGL* item = share->getItem("Share Group"))
            item->setToolTip(tag ? std::string("Only its members in this region get it.") : std::string("Wear a group tag to share with that group."));
    }
    if (LLMenuGL* show = menu->findChildMenuByName("Show", true))
    {
        const S32 blocked = gSavedPerAccountSettings.getLLSD("GMDXMapShareBlocked").size();
        show->setItemLabel("Unblock Map Shares", blocked ? llformat("Unblock All Map Shares (%d)", blocked) : std::string("Unblock All Map Shares"));
        show->setItemEnabled("Unblock Map Shares", blocked > 0);
    }
}

// ---- pings over the world -----------------------------------------------------------------------

void GMDXMapShare::drawWorld()
{
    static LLCachedControl<bool> enabled(gSavedSettings, "GMDXMapShareShow", true);
    if (!enabled || !gHUDView || sItems.empty()) return;
    const LLViewerCamera* camera = LLViewerCamera::getInstance();
    const LLRect rect = gHUDView->getRect();
    const LLFontGL* font = LLFontGL::getFontSansSerifSmallBold();
    for (const GMDXSharedItem& s : items())
    {
        if (s.type != Type::Ping || s.map_only) continue;
        // on screen where it is, else on the screen's edge towards it (shown through walls: the
        // sender chose to share the place)
        const LLVector3 agent = gAgent.getPosAgentFromGlobal(s.mark.pos_global);
        LLCoordGL screen;
        if (!camera->projectPosAgentToScreen(agent, screen, false) && !camera->projectPosAgentToScreenEdge(agent, screen)) continue;
        S32 x = 0, y = 0;
        gHUDView->screenPointToLocal(screen.mX, screen.mY, &x, &y);
        x = llclamp(x, 16, llmax(16, rect.getWidth() - 16));
        y = llclamp(y, 16, llmax(16, rect.getHeight() - 30));
        const F32 alpha = pingAlpha(s, false);   // (readable until its last 1.5 s)
        LLColor4 c = GMDXMinimapMarks::color(s.mark.color);
        c.mV[VALPHA] = alpha;
        const LLColor4 dark(0.05f, 0.06f, 0.07f, 0.85f * alpha);
        const F32 fx = (F32)x, fy = (F32)y;
        gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
        drawBlip(fx, fy, s, 34.f);   // a pulse when it appears
        for (int pass = 0; pass < 2; ++pass)   // a diamond: outline, then the colour
        {
            const F32 r = pass ? 6.f : 8.f;
            gGL.color4fv((pass ? c : dark).mV);
            gGL.begin(LLRender::TRIANGLES);
            gGL.vertex2f(fx, fy + r); gGL.vertex2f(fx - r, fy); gGL.vertex2f(fx + r, fy);
            gGL.vertex2f(fx - r, fy); gGL.vertex2f(fx, fy - r); gGL.vertex2f(fx + r, fy);
            gGL.end();
        }
        const F32 metres = dist_vec(gAgent.getPositionGlobal(), s.mark.pos_global);
        const std::string on = anchorName(s);
        const std::string label = llformat("%s%s  %s  %.0f m", s.mark.name.c_str(), on.empty() ? "" : (" on " + on).c_str(),
                                           senderName(s).c_str(), metres);
        LLColor4 text(0.95f, 0.96f, 0.97f, alpha);
        font->renderUTF8(label, 0, fx, fy - 11.f, text, LLFontGL::HCENTER, LLFontGL::TOP, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
    }
}

// ---- ping blips -------------------------------------------------------------------------------

F32 GMDXMapShare::pingAlpha(const GMDXSharedItem& item, bool on_map)
{
    static LLCachedControl<bool> blip(gSavedSettings, "GMDXPingBlip", true);
    const F64 t = now();
    if (on_map && blip && item.expires > item.arrived)
    {
        // a radar contact: bright, then fading smoothly away over the ping's life
        const F32 u = llclamp((F32)((t - item.arrived) / (item.expires - item.arrived)), 0.f, 1.f);
        return 1.f - u * u * (3.f - 2.f * u);
    }
    return llclamp((F32)(item.expires - t) / 1.5f, 0.f, 1.f);
}

void GMDXMapShare::drawBlip(F32 x, F32 y, const GMDXSharedItem& item, F32 max_radius)
{
    static LLCachedControl<bool> blip(gSavedSettings, "GMDXPingBlip", true);
    const F32 age = (F32)(now() - item.arrived);
    const LLColor4 c = GMDXMinimapMarks::color(item.mark.color);
    gGL.getTexUnit(0)->unbind(LLTexUnit::TT_TEXTURE);
    if (!blip)
    {
        if (age >= 2.f) return;   // one ring a second for its first 2 seconds
        const F32 phase = fmodf(age, 1.f);
        gGL.color4f(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], 1.f - phase);
        gl_circle_2d(x, y, 7.f + (max_radius - 7.f) * phase, 24, false);
        return;
    }
    // sonar: a flash, then three rings 0.35 s apart, each spreading out over 1.2 s and thinning away
    constexpr F32 FLASH = 0.25f, RING_GAP = 0.35f, RING_TIME = 1.2f;
    constexpr S32 RINGS = 3;
    if (age >= RING_GAP * (RINGS - 1) + RING_TIME) return;
    if (age < FLASH)
    {
        const F32 k = 1.f - age / FLASH;
        gGL.color4f(llmin(1.f, c.mV[VRED] + 0.4f), llmin(1.f, c.mV[VGREEN] + 0.4f), llmin(1.f, c.mV[VBLUE] + 0.4f), 0.85f * k);
        gl_circle_2d(x, y, 4.f + 6.f * k, 20, true);
    }
    for (S32 i = 0; i < RINGS; ++i)
    {
        const F32 phase = (age - RING_GAP * i) / RING_TIME;
        if (phase <= 0.f || phase >= 1.f) continue;
        const F32 fade = (1.f - phase) * (1.f - phase);
        gGL.color4f(c.mV[VRED], c.mV[VGREEN], c.mV[VBLUE], 0.9f * fade);
        gl_circle_2d(x, y, 4.f + (max_radius - 4.f) * phase, 28, false);
    }
}

// The pings on the World Map: a diamond in the ping's colour (its blip when it arrives), fading as
// on the mini-map, with what it is and who sent it.
void GMDXMapShare::drawWorldMap(LLWorldMapView* view)
{
    static LLCachedControl<bool> enabled(gSavedSettings, "GMDXMapShareShow", true);
    if (!enabled || !view || sItems.empty()) return;
    const LLFontGL* font = LLFontGL::getFontSansSerifSmallBold();
    const LLRect rect = view->getLocalRect();
    for (const GMDXSharedItem& s : sItems)
    {
        if (s.type != Type::Ping) continue;
        const LLVector3 p = view->globalPosToView(s.mark.pos_global);
        const F32 x = (F32)ll_round(p.mV[VX]), y = (F32)ll_round(p.mV[VY]);
        if (!rect.pointInRect((S32)x, (S32)y)) continue;
        const F32 alpha = pingAlpha(s, true);
        LLColor4 c = GMDXMinimapMarks::color(s.mark.color);
        c.mV[VALPHA] = alpha;
        drawBlip(x, y, s, 30.f);
        for (int pass = 0; pass < 2; ++pass)   // outline, then the colour
        {
            const F32 r = pass ? 4.5f : 6.f;
            gGL.color4fv((pass ? c : LLColor4(0.05f, 0.06f, 0.07f, 0.9f * alpha)).mV);
            gGL.begin(LLRender::TRIANGLES);
            gGL.vertex2f(x, y + r); gGL.vertex2f(x - r, y); gGL.vertex2f(x + r, y);
            gGL.vertex2f(x - r, y); gGL.vertex2f(x, y - r); gGL.vertex2f(x + r, y);
            gGL.end();
        }
        const std::string on = anchorName(s);
        LLColor4 text(0.95f, 0.96f, 0.97f, alpha);
        font->renderUTF8(s.mark.name + (on.empty() ? std::string() : " on " + on) + "  " + senderName(s), 0, x + 10.f, y, text,
                         LLFontGL::LEFT, LLFontGL::VCENTER, LLFontGL::NORMAL, LLFontGL::DROP_SHADOW);
    }
}
