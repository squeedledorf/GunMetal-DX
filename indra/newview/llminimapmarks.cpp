/**
 * @file llminimapmarks.cpp
 * @brief GunMetal-DX: named, coloured pins on the mini-map (see llminimapmarks.h).
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

// Ported from GunMetal-DX (squeedledorf, LGPL 2.1), v0.3.0 cf5f1f4. Whole file: see llminimapmarks.h.

#include "llviewerprecompiledheaders.h"

#include "llminimapmarks.h"

#include "lldir.h"
#include "llfile.h"
#include "llsdjson.h"
#include "llslurl.h"
#include "llsurface.h"
#include "llviewerregion.h"
#include "llworld.h"
#include "rlvactions.h"
#include "rlvcommon.h"

#include <boost/json.hpp>
#include <map>
#include <set>

namespace
{
bool save();

// A marks file as LLSD; undefined when it is missing or unreadable.
LLSD read_file(const std::string& path)
{
    llifstream f(path, std::ios::in | std::ios::binary);
    if (!f.is_open()) return LLSD();
    const std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    try { return LlsdFromJson(boost::json::parse(text)); }
    catch (...) { LL_WARNS("MinimapMarks") << "unreadable: " << path << LL_ENDL; return LLSD(); }
}

struct Store
{
    std::string account_dir;   // the account folder the path was made for (store() runs per avatar per frame)
    std::string path;   // the account file the marks came from ("" = not loaded)
    std::vector<LLMapMark> marks;
    std::vector<LLMapZone> zones;
    std::map<LLUUID, LLColor4> avatar_colors;
    std::string migrate_from;   // <GMDX> v0.3.0's file, merged in: set aside by the first save that lands
};

Store& store()
{
    static Store s;
    // (re)loaded for the account logged in now
    const std::string& account_dir = gDirUtilp->getLindenUserDir();
    if (account_dir == s.account_dir) return s;
    s.account_dir = account_dir;
    const std::string path = account_dir.empty() ? std::string()
                           : gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "minimap_marks.json");
    if (path == s.path) return s;
    s.path = path;
    s.marks.clear();
    s.zones.clear();
    s.avatar_colors.clear();
    if (path.empty()) return s;
    LLSD data = read_file(path);
    // <GMDX> v0.3.0 kept them in gmdx_minimap_marks.json: merged in once (this file wins on an id),
    // saved, then set aside as .migrated (save). Not over a file of this name that cannot be read:
    // that is left as it is.
    s.migrate_from.clear();
    const std::string old_path = gDirUtilp->getExpandedFilename(LL_PATH_PER_SL_ACCOUNT, "gmdx_minimap_marks.json");
    const bool can_merge = LLFile::isfile(old_path) && (data.isMap() || !LLFile::isfile(path));
    const LLSD old_data = can_merge ? read_file(old_path) : LLSD();
    if (old_data.isMap())
    {
        s.migrate_from = old_path;
        for (const char* list : { "marks", "zones", "avatar_colors" })
        {
            std::set<LLUUID> have;
            for (const LLSD& e : llsd::inArray(data[list])) have.insert(e["id"].asUUID());
            for (const LLSD& e : llsd::inArray(old_data[list]))
                if (!have.count(e["id"].asUUID())) data[list].append(e);
        }
    }
    // </GMDX>
    for (const LLSD& m : llsd::inArray(data["marks"]))
    {
        LLMapMark mark;
        mark.id = m["id"].asUUID();
        mark.name = m["name"].asString();
        mark.region = m["region"].asString();
        mark.pos_global.setVec(m["x"].asReal(), m["y"].asReal(), m["z"].asReal());
        mark.color = llclamp((S32)m["color"].asInteger(), 0, LLMinimapMarks::COLORS - 1);
        if (mark.id.isNull()) mark.id.generate();
        s.marks.push_back(mark);
    }
    for (const LLSD& z : llsd::inArray(data["zones"]))
    {
        LLMapZone zone;
        zone.id = z["id"].asUUID();
        zone.name = z["name"].asString();
        const std::string kind = z["kind"].asString();
        zone.kind = kind == "circle" ? LLMapZone::CIRCLE : kind == "polygon" ? LLMapZone::POLYGON : LLMapZone::RECT;
        for (const LLSD& p : llsd::inArray(z["points"]))
        {
            zone.points.emplace_back(p[0].asReal(), p[1].asReal(), 0.0);
        }
        zone.color = llclamp((S32)z["color"].asInteger(), 0, LLMinimapMarks::COLORS - 1);
        zone.dotted = z["dotted"].asBoolean();
        zone.fill = llclamp((S32)z["fill"].asInteger(), (S32)LLMapZone::FILL_NONE, (S32)LLMapZone::FILL_CAUTION);
        const size_t need = zone.kind == LLMapZone::POLYGON ? 3 : 2;
        if (zone.points.size() < need) continue;
        if (zone.id.isNull()) zone.id.generate();
        s.zones.push_back(zone);
    }
    for (const LLSD& a : llsd::inArray(data["avatar_colors"]))
    {
        const LLUUID id = a["id"].asUUID();
        const LLSD& c = a["color"];
        if (id.notNull() && c.isArray() && c.size() >= 3)
        {
            s.avatar_colors[id] = LLColor4((F32)c[0].asReal(), (F32)c[1].asReal(), (F32)c[2].asReal(), 1.f);
        }
    }
    if (!s.migrate_from.empty()) save();   // <GMDX>
    return s;
}

bool save()
{
    Store& s = store();
    if (s.path.empty()) return false;
    LLSD data = LLSD::emptyMap();
    LLSD& list = data["marks"] = LLSD::emptyArray();
    for (const LLMapMark& m : s.marks)
    {
        LLSD e;
        e["id"] = m.id;
        e["name"] = m.name;
        e["region"] = m.region;
        e["x"] = m.pos_global.mdV[VX];
        e["y"] = m.pos_global.mdV[VY];
        e["z"] = m.pos_global.mdV[VZ];
        e["color"] = m.color;
        list.append(e);
    }
    LLSD& zones = data["zones"] = LLSD::emptyArray();
    for (const LLMapZone& z : s.zones)
    {
        LLSD e;
        e["id"] = z.id;
        e["name"] = z.name;
        e["kind"] = z.kind == LLMapZone::CIRCLE ? "circle" : z.kind == LLMapZone::POLYGON ? "polygon" : "rect";
        LLSD& points = e["points"] = LLSD::emptyArray();
        for (const LLVector3d& p : z.points)
        {
            LLSD xy = LLSD::emptyArray();
            xy.append(p.mdV[VX]);
            xy.append(p.mdV[VY]);
            points.append(xy);
        }
        e["color"] = z.color;
        e["dotted"] = z.dotted;
        e["fill"] = z.fill;
        zones.append(e);
    }
    LLSD& avatars = data["avatar_colors"] = LLSD::emptyArray();
    for (const auto& [id, c] : s.avatar_colors)
    {
        LLSD e;
        e["id"] = id;
        LLSD& rgb = e["color"] = LLSD::emptyArray();
        rgb.append(c.mV[VRED]);
        rgb.append(c.mV[VGREEN]);
        rgb.append(c.mV[VBLUE]);
        avatars.append(e);
    }
    const std::string text = boost::json::serialize(LlsdToJson(data)) + "\n";
    const std::string tmp = s.path + ".tmp";
    {
        llofstream f(tmp, std::ios::out | std::ios::binary | std::ios::trunc);
        if (!f.is_open()) return false;
        f << text;
        f.close();
        if (f.fail()) { LLFile::remove(tmp, ENOENT); return false; }   // <GMDX> (a short write never replaces the file)
    }
    if (LLFile::replace(tmp, s.path) != 0) { LLFile::remove(tmp, ENOENT); return false; }
    // <GMDX> v0.3.0's file is in this one now: set aside (or else gone), so it never merges again
    if (!s.migrate_from.empty())
    {
        if (LLFile::replace(s.migrate_from, s.migrate_from + ".migrated") != 0) LLFile::remove(s.migrate_from, ENOENT);
        s.migrate_from.clear();
    }
    return true;
}

LLMapZone* find_zone_mutable(const LLUUID& id)
{
    for (LLMapZone& z : store().zones)
        if (z.id == id) return &z;
    return nullptr;
}

LLMapMark* find_mutable(const LLUUID& id)
{
    for (LLMapMark& m : store().marks)
        if (m.id == id) return &m;
    return nullptr;
}
} // namespace

LLColor4 LLMinimapMarks::color(S32 index)
{
    static const LLColor4 colors[COLORS] = {
        LLColor4(1.f, 0.353f, 0.353f, 1.f),     // red #FF5A5A
        LLColor4(1.f, 0.710f, 0.278f, 1.f),     // orange #FFB547
        LLColor4(0.961f, 0.843f, 0.431f, 1.f),  // yellow #F5D76E
        LLColor4(0.482f, 0.847f, 0.561f, 1.f),  // green #7BD88F
        LLColor4(0.435f, 0.827f, 1.f, 1.f),     // cyan #6FD3FF
        LLColor4(0.780f, 0.608f, 1.f, 1.f),     // purple #C79BFF
    };
    return colors[llclamp(index, 0, COLORS - 1)];
}

const char* LLMinimapMarks::colorName(S32 index)
{
    static const char* const names[COLORS] = { "Red", "Orange", "Yellow", "Green", "Cyan", "Purple" };
    return names[llclamp(index, 0, COLORS - 1)];
}

const std::vector<LLMapMark>& LLMinimapMarks::all()
{
    return store().marks;
}

const LLMapMark* LLMinimapMarks::find(const LLUUID& id)
{
    return find_mutable(id);
}

const LLMapMark& LLMinimapMarks::add(const LLVector3d& pos_global, const std::string& name, S32 color)
{
    LLMapMark m;
    m.id.generate();
    m.name = name;
    m.pos_global = pos_global;
    m.color = llclamp(color, 0, COLORS - 1);
    if (LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(pos_global))
    {
        m.region = region->getName();
    }
    store().marks.push_back(m);
    save();
    return store().marks.back();
}

void LLMinimapMarks::rename(const LLUUID& id, const std::string& name)
{
    if (LLMapMark* m = find_mutable(id)) { m->name = name; save(); }
}

void LLMinimapMarks::recolor(const LLUUID& id, S32 color)
{
    if (LLMapMark* m = find_mutable(id)) { m->color = llclamp(color, 0, COLORS - 1); save(); }
}

void LLMinimapMarks::remove(const LLUUID& id)
{
    auto& marks = store().marks;
    const size_t before = marks.size();
    marks.erase(std::remove_if(marks.begin(), marks.end(), [&id](const LLMapMark& m) { return m.id == id; }), marks.end());
    if (marks.size() != before) save();
}

const std::vector<LLMapZone>& LLMinimapMarks::zones()
{
    return store().zones;
}

const LLMapZone* LLMinimapMarks::findZone(const LLUUID& id)
{
    return find_zone_mutable(id);
}

void LLMinimapMarks::addZone(LLMapZone zone)
{
    if (zone.id.isNull()) zone.id.generate();
    zone.color = llclamp(zone.color, 0, COLORS - 1);
    store().zones.push_back(std::move(zone));
    save();
}

void LLMinimapMarks::renameZone(const LLUUID& id, const std::string& name)
{
    if (LLMapZone* z = find_zone_mutable(id)) { z->name = name; save(); }
}

void LLMinimapMarks::recolorZone(const LLUUID& id, S32 color)
{
    if (LLMapZone* z = find_zone_mutable(id)) { z->color = llclamp(color, 0, COLORS - 1); save(); }
}

void LLMinimapMarks::setZoneDotted(const LLUUID& id, bool dotted)
{
    if (LLMapZone* z = find_zone_mutable(id)) { z->dotted = dotted; save(); }
}

void LLMinimapMarks::setZoneFill(const LLUUID& id, S32 fill)
{
    if (LLMapZone* z = find_zone_mutable(id))
    {
        z->fill = llclamp(fill, (S32)LLMapZone::FILL_NONE, (S32)LLMapZone::FILL_CAUTION);
        save();
    }
}

void LLMinimapMarks::removeZone(const LLUUID& id)
{
    auto& zones = store().zones;
    const size_t before = zones.size();
    zones.erase(std::remove_if(zones.begin(), zones.end(), [&id](const LLMapZone& z) { return z.id == id; }), zones.end());
    if (zones.size() != before) save();
}

bool LLMinimapMarks::avatarColor(const LLUUID& avatar_id, LLColor4& color)
{
    const Store& s = store();
    const auto it = s.avatar_colors.find(avatar_id);
    if (it == s.avatar_colors.end()) return false;
    color = it->second;
    return true;
}

void LLMinimapMarks::setAvatarColor(const LLUUID& avatar_id, const LLColor4& color)
{
    if (avatar_id.isNull()) return;
    store().avatar_colors[avatar_id] = LLColor4(color.mV[VRED], color.mV[VGREEN], color.mV[VBLUE], 1.f);
    save();
}

void LLMinimapMarks::clearAvatarColor(const LLUUID& avatar_id)
{
    if (store().avatar_colors.erase(avatar_id)) save();
}

LLVector3d LLMinimapMarks::onGround(LLVector3d pos_global)
{
    if (LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(pos_global))
    {
        const LLVector3 p = region->getPosRegionFromGlobal(pos_global);
        pos_global.mdV[VZ] = region->getOriginGlobal().mdV[VZ] + region->getLand().resolveHeightRegion(p);
    }
    return pos_global;
}

std::string LLMinimapMarks::coordinatesText(const LLVector3d& pos_global)
{
    LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(pos_global);
    // [RLVa] the region's name, and global coordinates off the loaded regions, are hidden while the location is
    const bool show = RlvActions::canShowLocation();
    if (!region) return show ? llformat("%.0f, %.0f", pos_global.mdV[VX], pos_global.mdV[VY]) : RlvStrings::getString(RlvStringKeys::Hidden::Region);
    const LLVector3 p = region->getPosRegionFromGlobal(pos_global);
    const std::string name = show ? region->getName() : RlvStrings::getString(RlvStringKeys::Hidden::Region);
    return llformat("%s (%d, %d, %d)", name.c_str(), ll_round(p.mV[VX]), ll_round(p.mV[VY]), ll_round(p.mV[VZ]));
}

std::string LLMinimapMarks::slurl(const LLVector3d& pos_global)
{
    LLViewerRegion* region = LLWorld::getInstance()->getRegionFromPosGlobal(pos_global);
    if (!region || !RlvActions::canShowLocation()) return std::string();   // [RLVa] no SLURL while the location is hidden
    const LLVector3 p = region->getPosRegionFromGlobal(pos_global);
    return LLSLURL(region->getName(), LLVector3((F32)ll_round(p.mV[VX]), (F32)ll_round(p.mV[VY]), (F32)ll_round(p.mV[VZ])))
        .getSLURLString();
}
