/**
 * @file slparcelspeakers.cpp
 * @brief #892: the parcel speaker registry. See slparcelspeakers.h and
 *        doc/parcel_speakers_protocol.md.
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
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "slparcelspeakers.h"

#include "llagent.h"
#include "llframetimer.h"
#include "llparcel.h"
#include "llselectmgr.h"
#include "llviewercontrol.h"
#include "llviewerobject.h"
#include "llviewerobjectlist.h"
#include "llviewerparcelmgr.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "message.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cmath>
#include <unordered_map>
#include <unordered_set>

bool SLParcelSpeakers::sWatching = false;

namespace
{
constexpr F64 kHeavySecs = 0.25;            // registry pass cadence
constexpr F64 kRescanSecs = 15.0;           // full hover-text rescan: a safety net for updates the hooks miss
constexpr F64 kPropsRate = 10.0;            // properties requests per second (burst the same)
constexpr F64 kPropsTimeout = 10.0;         // no reply: retry
constexpr U32 kPropsTries = 3;              // ... this often, then back off
constexpr F64 kPropsBackoff = 120.0;
constexpr F64 kPropsStale = 300.0;          // re-verify owner / group this often anyway
constexpr F64 kPropsRetagFresh = 30.0;      // a retag re-verifies when the answer is older than this
constexpr F64 kParcelRate = 4.0;            // parcel-by-position requests per second
constexpr F64 kParcelTimeout = 10.0;
constexpr F64 kParcelTtl = 120.0;           // a cell's parcel answer is reused this long
constexpr U32 kMaxCells = 64;
constexpr U32 kDirtyMax = 1024;             // more pending hover-text changes: rescan instead
constexpr size_t kMaxGroupLen = 24;

// Parcel-by-position requests use sequence ids [kSeqBase - kSeqSlots + 1, kSeqBase]: between
// COLLISION_NOT_ON_LIST_PARCEL_SEQ_ID (-40000) and HOVERED_PARCEL_SEQ_ID (-50000), unused there.
constexpr S32 kSeqBase = -45000;
constexpr U32 kSeqSlots = 64;
static_assert(kSeqBase - (S32)kSeqSlots > HOVERED_PARCEL_SEQ_ID && kSeqBase < COLLISION_NOT_ON_LIST_PARCEL_SEQ_ID,
              "speaker parcel sequence ids must not collide with LL's");

using Tag = SLParcelSpeakers::Tag;
using Speaker = SLParcelSpeakers::Speaker;

// Owner / group from ObjectPropertiesFamily (sim-asserted), per LINKSET: keyed by the root prim
// (owner and group are the linkset's), so every tagged child of a linkset shares one answer and
// one request -- asked for the root, whatever the sim would answer for a child.
struct Ident
{
    bool known = false;
    LLUUID owner, group;
    F64  knownAt = 0.0;
    bool pending = false;
    bool refresh = false;       // re-verify soon (retag)
    F64  requestedAt = 0.0;
    F64  retryAt = 0.0;
    U32  tries = 0;
    F64  usedAt = 0.0;          // last pass a tagged prim needed it (unused ones are dropped)
};

// Why a tagged entry is not (or is) a speaker this pass; logged per prim when it changes.
enum EReason : U8
{
    R_NONE = 0, R_VERIFIED, R_PENDING, R_NO_IDENTITY, R_UNTRUSTED, R_OFF_PARCEL, R_PLACING, R_NOT_HERE, R_OFF,
};
const char* reasonName(U8 r)
{
    switch (r)
    {
    case R_VERIFIED:    return "speaker";
    case R_PENDING:     return "owner / group asked";
    case R_NO_IDENTITY: return "owner / group not answered (backing off)";
    case R_UNTRUSTED:   return "not trusted (owner / group)";
    case R_OFF_PARCEL:  return "off this parcel";
    case R_PLACING:     return "parcel lookup pending";
    case R_NOT_HERE:    return "in another region";
    case R_OFF:         return "tag says off";
    }
    return "-";
}

struct Entry
{
    Tag  tag;
    LLUUID root;                // the linkset root (the identity key); itself for a root prim
    bool child = false;
    // Last seen in the scene (object-cache culling can take a prim out of gObjectList while
    // it still exists; it keeps playing from here until it is back or its cache entry is gone).
    LLUUID regionId;
    U32  localId = 0;
    LLVector3d pos;
    LLQuaternion rot;
    bool seen = false;
    F64  goneSince = -1.0;      // not in gObjectList since
    F64  untaggedSince = -1.0;  // its text lost the tag since
    U8   reason = R_NONE;       // last logged
};

struct Cell
{
    bool known = false;
    S32  localId = -1;
    LLUUID owner;               // the parcel's OwnerID (the group for group-owned land)
    F64  expires = 0.0;
    S32  slot = -1;             // request in flight
    F64  requestedAt = 0.0;
};

struct Ctl
{
    bool enabled = false;
    std::unordered_map<LLUUID, Entry> entries;
    std::unordered_map<LLUUID, Ident> idents;       // key: linkset root id
    std::vector<LLUUID> dirty;
    std::unordered_set<LLUUID> dirtySet;
    bool rescan = false;

    // The agent's parcel as last seen.
    LLUUID regionId;
    S32    parcelId = -1;
    LLUUID parcelOwner, parcelGroup;
    std::string parcelDesc;
    SLParcelSpeakers::EOutside outside = SLParcelSpeakers::OUTSIDE_SILENT;

    std::unordered_map<U32, Cell> cells;            // key: row << 16 | col, agent region only
    std::array<S64, kSeqSlots> slotCell;            // cell key per in-flight slot, -1 free

    std::vector<Speaker> out;
    U32 generation = 0;

    F64 nextHeavy = 0.0;
    F64 propsTokens = kPropsRate, parcelTokens = kParcelRate, lastTokens = 0.0;
    SLParcelSpeakers::Stats stats;
    std::string lastSummary;

    Ctl() { slotCell.fill(-1); stats.cap = SLParcelSpeakers::kMaxCandidates; }
};
Ctl& ctl()
{
    static Ctl c;
    return c;
}

char lower(char ch) { return (char)std::tolower((unsigned char)ch); }

// The first "[slspk" (any case) followed by a space or ']' at or after `from`. npos: none.
size_t findTagStart(const std::string& s, size_t from = 0)
{
    static const char kPrefix[] = "[slspk";
    constexpr size_t n = sizeof(kPrefix) - 1;
    for (size_t i = s.find('[', from); i != std::string::npos && i + n <= s.size(); i = s.find('[', i + 1))
    {
        size_t k = 1;
        while (k < n && lower(s[i + k]) == kPrefix[k]) ++k;
        if (k < n) continue;
        if (i + n == s.size()) return std::string::npos;   // unterminated
        const char after = s[i + n];
        if (after == ']' || std::isspace((unsigned char)after)) return i;
    }
    return std::string::npos;
}

// The tag's body (between "[slspk" and ']'), lower-cased. false: no (terminated) tag.
bool tagBody(const std::string& text, std::string& body)
{
    const size_t at = findTagStart(text);
    if (at == std::string::npos) return false;
    const size_t end = text.find(']', at);
    if (end == std::string::npos) return false;
    body = text.substr(at + 6, end - at - 6);
    std::transform(body.begin(), body.end(), body.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    return true;
}

// Space-separated tokens.
template <typename F> void forTokens(const std::string& body, F&& fn)
{
    size_t i = 0;
    while (i < body.size())
    {
        while (i < body.size() && std::isspace((unsigned char)body[i])) ++i;
        size_t j = i;
        while (j < body.size() && !std::isspace((unsigned char)body[j])) ++j;
        if (j == i) break;
        fn(body.substr(i, j - i));
        i = j;
    }
}

bool parseF32(const std::string& v, F32& out)
{
    const char* b = v.data();
    const char* e = b + v.size();
    F32 x = 0.f;
    const auto r = std::from_chars(b, e, x);   // (locale-independent: '.' always)
    if (r.ec != std::errc() || r.ptr != e || !std::isfinite(x)) return false;
    out = x;
    return true;
}

bool parseS64(const std::string& v, S64& out)
{
    const char* b = v.data();
    const char* e = b + v.size();
    S64 x = 0;
    const auto r = std::from_chars(b, e, x, 10);
    if (r.ec != std::errc() || r.ptr != e) return false;
    out = x;
    return true;
}

bool validGroupName(const std::string& g)
{
    if (g.empty() || g.size() > kMaxGroupLen) return false;
    return std::all_of(g.begin(), g.end(), [](unsigned char ch)
                       { return std::isalnum(ch) || ch == '_' || ch == '-' || ch == '.'; });
}

bool isExcluded(LLViewerObject* obj)
{
    if (!obj || obj->isDead() || obj->isAvatar() || obj->isAttachment() || obj->isHUDAttachment()) return true;
    LLViewerObject* root = obj->getRootEdit();
    return root && (root->isAvatar() || root->isAttachment() || root->isHUDAttachment());
}

void markDirty(Ctl& c, const LLUUID& id)
{
    if (c.rescan) return;   // (a rescan will see it anyway)
    if (c.dirtySet.size() >= kDirtyMax)
    {
        c.dirty.clear();
        c.dirtySet.clear();
        c.rescan = true;
        return;
    }
    if (c.dirtySet.insert(id).second) c.dirty.push_back(id);
}

void clearAll(Ctl& c)
{
    c.entries.clear();
    c.idents.clear();
    c.dirty.clear();
    c.dirtySet.clear();
    c.cells.clear();
    c.slotCell.fill(-1);
    if (!c.out.empty()) ++c.generation;
    c.out.clear();
    c.regionId.setNull();
    c.parcelId = -1;
    c.parcelOwner.setNull();
    c.parcelGroup.setNull();
    c.parcelDesc.clear();
    c.outside = SLParcelSpeakers::OUTSIDE_SILENT;
    c.lastSummary.clear();
    const U32 peak = c.stats.peak, overflow = c.stats.overflow, requests = c.stats.requests, lookups = c.stats.parcelLookups;
    c.stats = SLParcelSpeakers::Stats();
    c.stats.cap = SLParcelSpeakers::kMaxCandidates;
    c.stats.peak = peak;
    c.stats.overflow = overflow;
    c.stats.requests = requests;
    c.stats.parcelLookups = lookups;
}

constexpr F64 kTagLostGrace = 5.0;      // a tag that vanished is kept this long (an update may restore it)
constexpr F64 kGoneMax = 600.0;         // a culled prim with a cache entry is kept at most this long

LLViewerObject* linksetRoot(LLViewerObject* obj)
{
    LLViewerObject* r = obj ? obj->getRootEdit() : nullptr;
    return r ? r : obj;
}

// Where it is now (and which linkset it belongs to).
void noteSeen(Entry& e, LLViewerObject* obj)
{
    LLViewerObject* r = linksetRoot(obj);
    e.root = r->getID();
    e.child = r != obj;
    e.regionId = obj->getRegion() ? obj->getRegion()->getRegionID() : LLUUID::null;
    e.localId = obj->getLocalID();
    e.pos = obj->getPositionGlobal();
    e.rot = obj->getRotationRegion();
    e.seen = true;
    e.goneSince = -1.0;
}

// (Re)read one object's hover text into the table.
void processObject(Ctl& c, const LLUUID& id, F64 now)
{
    LLViewerObject* obj = gObjectList.findObject(id);
    auto it = c.entries.find(id);
    if (!obj || obj->isDead())
    {
        // Out of the scene (killed, or culled into the object cache): heavyPass decides.
        if (it != c.entries.end() && it->second.goneSince < 0.0) it->second.goneSince = now;
        return;
    }
    if (isExcluded(obj))
    {
        if (it != c.entries.end())
        {
            LL_INFOS("ParcelAudio") << "speaker " << id << " dropped: now part of an avatar / attachment" << LL_ENDL;
            c.entries.erase(it);
        }
        return;
    }
    Tag tag;
    std::string bad;
    if (!SLParcelSpeakers::parseTag(obj->mHudText, tag, &bad))
    {
        // The tag is gone from the text. Kept kTagLostGrace in case an update without the text
        // field took it and the next one brings it back (logged either way: a child prim whose
        // linkset update arrives without text shows up here).
        if (it != c.entries.end() && it->second.untaggedSince < 0.0)
        {
            it->second.untaggedSince = now;
            LL_INFOS("ParcelAudio") << "speaker " << id << (it->second.child ? " (child prim)" : " (root prim)")
                                    << " lost its tag (text now " << (obj->mHudText.empty() ? "empty" : "other")
                                    << "): kept " << (S32)kTagLostGrace << " s in case it returns" << LL_ENDL;
        }
        return;
    }
    if (it == c.entries.end())
    {
        if (c.entries.size() >= SLParcelSpeakers::kMaxCandidates)
        {
            ++c.stats.overflow;
            return;
        }
        Entry e;
        e.tag = tag;
        noteSeen(e, obj);
        LL_INFOS("ParcelAudio") << "speaker candidate " << id << (e.child ? " (child of " + e.root.asString() + ")" : " (root prim)")
                                << " grp " << tag.group << " vol " << tag.vol << " r " << tag.range << (tag.off ? " (off)" : "")
                                << (bad.empty() ? "" : "; not understood: ") << bad << LL_ENDL;
        c.entries.emplace(id, e);
        return;
    }
    Entry& e = it->second;
    if (e.untaggedSince >= 0.0)
    {
        LL_INFOS("ParcelAudio") << "speaker " << id << " got its tag back after "
                                << llformat("%.1f", now - e.untaggedSince) << " s" << LL_ENDL;
        e.untaggedSince = -1.0;
    }
    noteSeen(e, obj);
    if (e.tag != tag)
    {
        e.tag = tag;
        // Live scripts retag often (level changes): re-verify only when the answer is not fresh.
        auto ii = c.idents.find(e.root);
        if (ii != c.idents.end() && ii->second.known && now - ii->second.knownAt > kPropsRetagFresh) ii->second.refresh = true;
    }
}

// Ask the sim for the linkset's owner / group: for its root prim.
void requestProperties(Ctl& c, LLViewerObject* root, Ident& id, F64 now)
{
    if (!root || !root->getRegion()) return;
    LLSelectMgr::getInstance()->requestObjectPropertiesFamily(root);
    id.pending = true;
    id.requestedAt = now;
    ++id.tries;
    c.propsTokens -= 1.0;
    ++c.stats.requests;
}

U32 cellKey(const LLVector3& pos_region, F32 width)
{
    const S32 max_idx = std::max(1, (S32)(width / PARCEL_GRID_STEP_METERS)) - 1;
    const S32 col = llclamp((S32)std::floor(pos_region.mV[VX] / PARCEL_GRID_STEP_METERS), 0, max_idx);
    const S32 row = llclamp((S32)std::floor(pos_region.mV[VY] / PARCEL_GRID_STEP_METERS), 0, max_idx);
    return ((U32)row << 16) | (U32)col;
}

void requestCell(Ctl& c, LLViewerRegion* region, U32 key, Cell& cell, F64 now)
{
    S32 slot = -1;
    for (U32 i = 0; i < kSeqSlots; ++i)
    {
        if (c.slotCell[i] < 0) { slot = (S32)i; break; }
    }
    if (slot < 0) return;
    const F32 west = PARCEL_GRID_STEP_METERS * (F32)(key & 0xffff);
    const F32 south = PARCEL_GRID_STEP_METERS * (F32)(key >> 16);

    LLMessageSystem* msg = gMessageSystem;
    msg->newMessageFast(_PREHASH_ParcelPropertiesRequest);
    msg->nextBlockFast(_PREHASH_AgentData);
    msg->addUUIDFast(_PREHASH_AgentID, gAgent.getID());
    msg->addUUIDFast(_PREHASH_SessionID, gAgent.getSessionID());
    msg->nextBlockFast(_PREHASH_ParcelData);
    msg->addS32Fast(_PREHASH_SequenceID, kSeqBase - slot);
    msg->addF32Fast(_PREHASH_West, west);
    msg->addF32Fast(_PREHASH_South, south);
    msg->addF32Fast(_PREHASH_East, west + PARCEL_GRID_STEP_METERS);
    msg->addF32Fast(_PREHASH_North, south + PARCEL_GRID_STEP_METERS);
    msg->addBOOL("SnapSelection", false);
    msg->sendReliable(region->getHost());

    c.slotCell[slot] = (S64)key;
    cell.slot = slot;
    cell.requestedAt = now;
    c.parcelTokens -= 1.0;
    ++c.stats.parcelLookups;
}

// Whether a trusted speaker at `pos_global` stands where it may play this parcel's stream.
// 0: no, 1: yes, -1: not known yet (a lookup is pending).
S32 onParcel(Ctl& c, LLViewerRegion* region, const LLVector3d& pos_global, F64 now)
{
    const LLVector3 pos_region = region->getPosRegionFromGlobal(pos_global);
    const F32 width = region->getWidth();
    if (pos_region.mV[VX] < 0.f || pos_region.mV[VY] < 0.f || pos_region.mV[VX] >= width || pos_region.mV[VY] >= width)
        return 0;   // another region: v1 plays the agent's parcel only
    if (LLViewerParcelMgr::getInstance()->inAgentParcel(pos_global))
        return 1;

    // Another parcel: it counts only with the same owner (internal parcel lines on a stage).
    const U32 key = cellKey(pos_region, width);
    auto it = c.cells.find(key);
    if (it == c.cells.end())
    {
        if (c.cells.size() >= kMaxCells)
        {
            // Evict an expired / unanswered cell; none: wait.
            auto victim = std::find_if(c.cells.begin(), c.cells.end(), [now](const auto& kv)
                                       { return kv.second.slot < 0 && (!kv.second.known || kv.second.expires < now); });
            if (victim == c.cells.end()) return -1;
            c.cells.erase(victim);
        }
        it = c.cells.emplace(key, Cell()).first;
    }
    Cell& cell = it->second;
    if (cell.slot >= 0 && now - cell.requestedAt > kParcelTimeout)
    {
        c.slotCell[cell.slot] = -1;
        cell.slot = -1;
    }
    if (cell.known && cell.expires >= now)
        return (cell.owner.notNull() && cell.owner == c.parcelOwner) ? 1 : 0;
    if (cell.slot < 0 && c.parcelTokens >= 1.0)
        requestCell(c, region, key, cell, now);
    // An expired answer still stands until the new one arrives.
    if (cell.known)
        return (cell.owner.notNull() && cell.owner == c.parcelOwner) ? 1 : 0;
    return -1;
}

void heavyPass(Ctl& c, F64 now)
{
    LLViewerRegion* region = gAgent.getRegion();
    LLParcel* parcel = LLViewerParcelMgr::getInstance()->getAgentParcel();

    // The agent's parcel: rebuild on any change.
    if (!region || !parcel)
    {
        if (!c.out.empty()) ++c.generation;
        c.out.clear();
        return;
    }
    if (region->getRegionID() != c.regionId)
    {
        c.cells.clear();
        c.slotCell.fill(-1);
    }
    const bool parcel_changed = region->getRegionID() != c.regionId || parcel->getLocalID() != c.parcelId
                                || parcel->getOwnerID() != c.parcelOwner || parcel->getGroupID() != c.parcelGroup;
    if (parcel_changed || parcel->getDesc() != c.parcelDesc)
    {
        if (parcel->getOwnerID() != c.parcelOwner)
        {
            c.cells.clear();   // (the cached answers compare against the old owner)
            c.slotCell.fill(-1);
        }
        c.regionId = region->getRegionID();
        c.parcelId = parcel->getLocalID();
        c.parcelOwner = parcel->getOwnerID();
        c.parcelGroup = parcel->getGroupID();
        c.parcelDesc = parcel->getDesc();
        std::string bad;
        SLParcelSpeakers::EOutside policy = SLParcelSpeakers::OUTSIDE_SILENT;
        const bool have = SLParcelSpeakers::parsePolicy(c.parcelDesc, policy, &bad);
        c.outside = policy;
        if (have)
            LL_INFOS("ParcelAudio") << "parcel speaker policy: outside="
                                    << (policy == SLParcelSpeakers::OUTSIDE_AMBIENT ? "ambient" : "silent")
                                    << (bad.empty() ? "" : "; not understood: ") << bad << LL_ENDL;
    }

    // Hover-text changes since the last pass (or everything, after enabling / an overflow, and
    // every kRescanSecs: copies / selections can arrive by paths that skip the text hooks).
    static F64 s_lastRescan = 0.0;
    if (now - s_lastRescan > kRescanSecs)
    {
        s_lastRescan = now;
        c.rescan = true;
    }
    if (c.rescan)
    {
        c.rescan = false;
        c.dirty.clear();
        c.dirtySet.clear();
        const S32 n = gObjectList.getNumObjects();
        U32 with_text = 0, tagged = 0;
        std::string sample;
        for (S32 i = 0; i < n; ++i)
        {
            LLViewerObject* obj = gObjectList.getObject(i);
            if (!obj || obj->isDead() || obj->mHudText.empty()) continue;
            ++with_text;
            if (sample.size() < 400 && obj->mHudText.find("slspk") != std::string::npos)
                sample += " {" + obj->getID().asString().substr(0, 8) + ": " + obj->mHudText.substr(0, 48) + "}";
            if (SLParcelSpeakers::hasTagPrefix(obj->mHudText))
            {
                ++tagged;
                processObject(c, obj->getID(), now);
            }
        }
        // (#892 diagnosis: how many objects carry hover text at all, and the slspk ones seen.)
        LL_INFOS("ParcelAudio") << "rescan: " << n << " objects, " << with_text << " with hover text, " << tagged
                                << " tagged;" << (sample.empty() ? " no slspk text seen" : sample) << LL_ENDL;
    }
    if (!c.dirty.empty())
    {
        std::vector<LLUUID> dirty;
        dirty.swap(c.dirty);
        c.dirtySet.clear();
        for (const LLUUID& id : dirty)
            processObject(c, id, now);
    }

    // Rate-limit tokens.
    const F64 dt = llclamp(now - c.lastTokens, 0.0, 1.0);
    c.lastTokens = now;
    c.propsTokens = std::min(kPropsRate, c.propsTokens + dt * kPropsRate);
    c.parcelTokens = std::min(kParcelRate, c.parcelTokens + dt * kParcelRate);

    std::vector<Speaker> out;
    SLParcelSpeakers::Stats& st = c.stats;
    st.pending = st.untrusted = st.offParcel = 0;
    st.notHere = st.noIdentity = st.placing = st.off = st.culled = st.tagLost = 0;
    for (auto it = c.entries.begin(); it != c.entries.end();)
    {
        const LLUUID& id = it->first;
        Entry& e = it->second;
        LLViewerObject* obj = gObjectList.findObject(id);
        if (obj && obj->isDead()) obj = nullptr;
        if (obj && isExcluded(obj))
        {
            LL_INFOS("ParcelAudio") << "speaker " << id << " dropped: now part of an avatar / attachment" << LL_ENDL;
            it = c.entries.erase(it);
            continue;
        }
        if (obj)
        {
            noteSeen(e, obj);
        }
        else
        {
            // Not in the scene. Object-cache culling removes prims the camera does not need
            // (behind it, far) but keeps their cache entry -- a speaker must not go silent for
            // that: it plays on from where it was seen. Killed by the sim (deleted, taken): the
            // cache entry goes too, and so does the speaker.
            if (e.goneSince < 0.0) e.goneSince = now;
            LLViewerRegion* er = e.regionId.notNull() ? LLWorld::getInstance()->getRegionFromID(e.regionId) : nullptr;
            const bool cached = er && e.localId && er->getCacheEntry(e.localId) != nullptr;   // (a kill invalidates it)
            if (!e.seen || !cached || now - e.goneSince > kGoneMax)
            {
                LL_INFOS("ParcelAudio") << "speaker " << id << (e.child ? " (child prim)" : "") << " dropped: "
                                        << (cached ? "out of the scene too long" : "object gone (killed / out of range)") << LL_ENDL;
                it = c.entries.erase(it);
                continue;
            }
            ++st.culled;
        }
        if (e.untaggedSince >= 0.0)
        {
            if (now - e.untaggedSince >= kTagLostGrace)
            {
                LL_INFOS("ParcelAudio") << "speaker " << id << (e.child ? " (child prim)" : "") << " dropped: tag removed" << LL_ENDL;
                it = c.entries.erase(it);
                continue;
            }
            ++st.tagLost;   // (keeps playing meanwhile)
        }
        const bool here = e.regionId == region->getRegionID();

        // Owner / group of the LINKSET (asked for its root): once, retry lost replies, back off,
        // re-verify when stale or retagged.
        Ident& idn = c.idents[e.root];
        idn.usedAt = now;
        if (idn.pending && now - idn.requestedAt > kPropsTimeout)
        {
            idn.pending = false;
            if (idn.tries >= kPropsTries)
            {
                idn.tries = 0;
                idn.retryAt = now + kPropsBackoff;
                LL_INFOS("ParcelAudio") << "speaker linkset " << e.root << ": no owner / group answer after " << kPropsTries
                                        << " requests; retrying in " << (S32)kPropsBackoff << " s" << LL_ENDL;
            }
        }
        const bool want = !idn.known || idn.refresh || now - idn.knownAt > kPropsStale;
        LLViewerObject* root_obj = gObjectList.findObject(e.root);
        if (root_obj && root_obj->isDead()) root_obj = nullptr;
        if (here && want && !idn.pending && now >= idn.retryAt && c.propsTokens >= 1.0 && root_obj)
            requestProperties(c, root_obj, idn, now);

        U8 reason = R_NONE;
        if (!here) reason = R_NOT_HERE;
        else if (!idn.known) reason = idn.pending ? R_PENDING : R_NO_IDENTITY;
        else if (e.tag.off) reason = R_OFF;
        else
        {
            const bool trusted = (c.parcelOwner.notNull() && idn.owner == c.parcelOwner)              // the parcel owner's
                                 || (c.parcelGroup.notNull() && idn.owner == c.parcelGroup)           // deeded to the parcel group
                                 || (c.parcelGroup.notNull() && idn.group == c.parcelGroup);          // set to the parcel group
            if (!trusted)
            {
                reason = R_UNTRUSTED;
                LL_INFOS_ONCE("ParcelAudio") << "speaker " << id << " not trusted: linkset owner " << idn.owner << " group "
                                             << idn.group << " vs parcel owner " << c.parcelOwner << " group " << c.parcelGroup
                                             << LL_ENDL;
            }
            else
            {
                const S32 placed = onParcel(c, region, e.pos, now);
                if (placed == 1)
                {
                    reason = R_VERIFIED;
                    out.push_back({ id, e.pos, e.rot, e.tag });
                }
                else reason = placed == 0 ? R_OFF_PARCEL : R_PLACING;
            }
        }
        switch (reason)
        {
        case R_PENDING:     ++st.pending; break;
        case R_NO_IDENTITY: ++st.noIdentity; break;
        case R_UNTRUSTED:   ++st.untrusted; break;
        case R_OFF_PARCEL:  ++st.offParcel; break;
        case R_PLACING:     ++st.placing; break;
        case R_NOT_HERE:    ++st.notHere; break;
        case R_OFF:         ++st.off; break;
        default: break;
        }
        if (reason != e.reason)
        {
            // Each prim's state, as it changes (a tagged prim that does not play says why).
            if (reason != R_PENDING)
                LL_INFOS("ParcelAudio") << "speaker " << id << (e.child ? " (child of " + e.root.asString() + ")" : " (root)")
                                        << ": " << reasonName(reason) << LL_ENDL;
            e.reason = reason;
        }
        ++it;
    }
    // Identities no tagged prim needed for a while.
    for (auto ii = c.idents.begin(); ii != c.idents.end();)
    {
        if (now - ii->second.usedAt > kPropsStale && !ii->second.pending) ii = c.idents.erase(ii);
        else ++ii;
    }
    std::sort(out.begin(), out.end(), [](const Speaker& a, const Speaker& b) { return a.id < b.id; });

    // Membership or a tag changed: new generation (positions alone do not count).
    bool same = out.size() == c.out.size();
    for (size_t i = 0; same && i < out.size(); ++i)
        same = out[i].id == c.out[i].id && out[i].tag == c.out[i].tag;
    if (!same) ++c.generation;
    c.out.swap(out);

    st.live = (U32)c.entries.size();
    st.peak = std::max(st.peak, st.live);
    st.verified = (U32)c.out.size();
    st.cells = (U32)c.cells.size();

    const std::string summary = llformat("%u speakers on parcel %d (%u tagged nearby: %u untrusted, %u off-parcel, %u pending, "
                                         "%u no owner answer, %u parcel lookup, %u other region, %u off; %u culled, %u tag lost; peak %u/%u)",
                                         st.verified, c.parcelId, st.live, st.untrusted, st.offParcel, st.pending, st.noIdentity,
                                         st.placing, st.notHere, st.off, st.culled, st.tagLost, st.peak, st.cap);
    // Log settled states (and every membership change), not every request in flight.
    // (A lookup that never settles must still show up: also log a changed summary every 10 s.)
    static F64 s_lastForced = 0.0;
    const bool force = summary != c.lastSummary && now - s_lastForced > 10.0;
    if (force) s_lastForced = now;
    if (summary != c.lastSummary && (!same || st.pending == 0 || force))
    {
        LL_INFOS("ParcelAudio") << summary << LL_ENDL;
        c.lastSummary = summary;
    }
}
} // namespace

// static
bool SLParcelSpeakers::hasTagPrefix(const std::string& text)
{
    return findTagStart(text) != std::string::npos;
}

// static
std::string SLParcelSpeakers::displayText(const std::string& text)
{
    const size_t at = findTagStart(text);
    if (at == std::string::npos) return text;
    const size_t end = text.find(']', at);
    if (end == std::string::npos) return text;
    std::string out = text.substr(0, at) + text.substr(end + 1);
    // Drop the whitespace / blank line the tag leaves behind.
    const size_t first = out.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    const size_t last = out.find_last_not_of(" \t\r\n");
    return out.substr(first, last - first + 1);
}

// static
bool SLParcelSpeakers::parseTag(const std::string& text, Tag& out, std::string* errors)
{
    std::string body;
    if (!tagBody(text, body)) return false;
    Tag t;
    std::string bad;
    forTokens(body, [&](const std::string& tok)
    {
        if (tok == "off")
        {
            t.off = true;
            return;
        }
        const size_t eq = tok.find('=');
        if (eq == std::string::npos || eq == 0)
        {
            bad += (bad.empty() ? "" : " ") + tok;
            return;
        }
        const std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
        bool ok = true;
        F32 f = 0.f;
        if (k == "grp")
        {
            ok = validGroupName(v);
            if (ok) t.group = v;
        }
        else if (k == "ch")
        {
            if (v == "m") t.role = SLParcelSpeakers::ROLE_MONO;
            else if (v == "l") t.role = SLParcelSpeakers::ROLE_LEFT;
            else if (v == "r") t.role = SLParcelSpeakers::ROLE_RIGHT;
            else if (v == "s") t.role = SLParcelSpeakers::ROLE_SUB;
            else ok = false;
        }
        else if (k == "vol")
        {
            ok = parseF32(v, f);
            if (ok) t.vol = llclamp(f, 0.f, SLParcelSpeakers::kMaxVol);
        }
        else if (k == "r")
        {
            ok = parseF32(v, f);
            if (ok) t.range = llclamp(f, 5.f, 100.f);
        }
        else if (k == "dir")
        {
            ok = parseF32(v, f);
            if (ok)
            {
                t.dirDeg = llclamp(f, 1.f, 360.f);
                t.hasDir = t.dirDeg < 360.f;
            }
        }
        else if (k == "t0")
        {
            S64 s = 0;
            // Unix seconds, 2000-01-01 .. 2100-01-01: anything else is a typo, not a clip start.
            ok = parseS64(v, s) && s >= 946684800LL && s < 4102444800LL;
            if (ok)
            {
                t.t0 = s;
                t.hasT0 = true;
            }
        }
        // (Unknown keys are ignored silently: later versions may add some.)
        if (!ok) bad += (bad.empty() ? "" : " ") + tok;
    });
    out = t;
    if (errors) *errors = bad;
    return true;
}

// static
bool SLParcelSpeakers::parsePolicy(const std::string& desc, EOutside& out, std::string* errors)
{
    std::string body;
    if (!tagBody(desc, body)) return false;
    EOutside policy = OUTSIDE_SILENT;
    std::string bad;
    forTokens(body, [&](const std::string& tok)
    {
        const size_t eq = tok.find('=');
        if (eq == std::string::npos) return;   // (bare words: e.g. the retired "group-ok"; ignored)
        const std::string k = tok.substr(0, eq), v = tok.substr(eq + 1);
        if (k != "outside") return;
        if (v == "silent") policy = OUTSIDE_SILENT;
        else if (v == "ambient") policy = OUTSIDE_AMBIENT;
        else bad += (bad.empty() ? "" : " ") + tok;
    });
    out = policy;
    if (errors) *errors = bad;
    return true;
}

// static
void SLParcelSpeakers::onHoverText(LLViewerObject* obj)
{
    if (!sWatching || !obj) return;
    Ctl& c = ctl();
    const LLUUID& id = obj->getID();
    // Only tags matter, and objects we track (their tag may just have been removed).
    if (c.entries.count(id) || (!obj->mHudText.empty() && hasTagPrefix(obj->mHudText)))
        markDirty(c, id);
}

// static
bool SLParcelSpeakers::onPropertiesFamily(const LLUUID& object_id, const LLUUID& owner_id, const LLUUID& group_id)
{
    if (!sWatching) return false;
    Ctl& c = ctl();
    // Asked for linkset roots (Ident is per root).
    auto it = c.idents.find(object_id);
    if (it == c.idents.end() || (!it->second.pending && it->second.tries == 0)) return false;
    Ident& e = it->second;
    const bool changed = !e.known || e.owner != owner_id || e.group != group_id;
    e.known = true;
    e.owner = owner_id;
    e.group = group_id;
    e.knownAt = LLFrameTimer::getElapsedSeconds();
    e.pending = false;
    e.refresh = false;
    e.tries = 0;
    e.retryAt = 0.0;
    if (changed)
        LL_INFOS("ParcelAudio") << "speaker linkset " << object_id << " owner " << owner_id << " group " << group_id << LL_ENDL;
    return true;
}

// static
bool SLParcelSpeakers::isSpeakerParcelSeq(S32 sequence_id)
{
    return sequence_id <= kSeqBase && sequence_id > kSeqBase - (S32)kSeqSlots;
}

// static
void SLParcelSpeakers::onParcelProperties(S32 sequence_id, const LLViewerRegion* region, S32 local_id,
                                          const LLUUID& owner_id, const LLUUID& group_id, bool group_owned)
{
    if (!isSpeakerParcelSeq(sequence_id)) return;
    Ctl& c = ctl();
    const U32 slot = (U32)(kSeqBase - sequence_id);
    const S64 key = c.slotCell[slot];
    c.slotCell[slot] = -1;
    if (key < 0 || !region || region->getRegionID() != c.regionId) return;   // stale (region changed)
    auto it = c.cells.find((U32)key);
    if (it == c.cells.end() || it->second.slot != (S32)slot) return;
    Cell& cell = it->second;
    cell.slot = -1;
    cell.known = true;
    cell.localId = local_id;
    cell.owner = owner_id;   // (group-owned land: OwnerID is the group, as for the agent parcel)
    cell.expires = LLFrameTimer::getElapsedSeconds() + kParcelTtl;
    (void)group_id;
    (void)group_owned;
}

// static
void SLParcelSpeakers::update()
{
    static LLCachedControl<bool> s_enabled(gSavedSettings, "ParcelAudioSpeakers", false);
    Ctl& c = ctl();
    if (!s_enabled)
    {
        if (c.enabled)
        {
            clearAll(c);
            c.enabled = false;
            sWatching = false;
            LL_INFOS("ParcelAudio") << "parcel speakers off" << LL_ENDL;
        }
        return;
    }
    if (!c.enabled)
    {
        c.enabled = true;
        sWatching = true;
        c.rescan = true;   // tags that arrived while we were not listening
        c.nextHeavy = 0.0;
        LL_INFOS("ParcelAudio") << "parcel speakers on" << LL_ENDL;
    }

    const F64 now = LLFrameTimer::getElapsedSeconds();
    if (now >= c.nextHeavy)
    {
        c.nextHeavy = now + kHeavySecs;
        heavyPass(c, now);
        return;
    }
    // Between passes: fresh positions only (speakers on moving vehicles / stages).
    for (Speaker& s : c.out)
    {
        LLViewerObject* obj = gObjectList.findObject(s.id);
        if (obj && !obj->isDead())
        {
            s.posGlobal = obj->getPositionGlobal();   // (a child: its own place in the linkset)
            s.rotation = obj->getRotationRegion();
        }
    }
}

// static
const std::vector<SLParcelSpeakers::Speaker>& SLParcelSpeakers::speakers()
{
    return ctl().out;
}

// static
U32 SLParcelSpeakers::generation()
{
    return ctl().generation;
}

// static
SLParcelSpeakers::EOutside SLParcelSpeakers::outsidePolicy()
{
    return ctl().outside;
}

// static
const SLParcelSpeakers::Stats& SLParcelSpeakers::stats()
{
    return ctl().stats;
}
