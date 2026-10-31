/**
 * @file gmdxslmcfactions.cpp
 * @brief The SLMC faction table: a faction's main, militia and land groups are one side.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2026, Martin "Nexii" Pitt <martin.r.pitt@gmail.com>
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

// <GMDX> From Alchemy: Skooma (Nexii Malthus), LGPL 2.1: the SLMC faction table and its loader
// (skmapallies.cpp FactionTable::fromLLSD / load / factionOf, 86d86a029d). See gmdxslmcfactions.h.

#include "llviewerprecompiledheaders.h"

#include "gmdxslmcfactions.h"

#include "lldir.h"
#include "llsdserialize.h"

#include <algorithm>
#include <unordered_map>
#include <vector>

namespace
{
    constexpr char FACTIONS_FILE[] = "gmdx_slmc_factions.xml";

    // Each faction's g, m and l keys hold a uuid, a string or an array of either; null and
    // invalid ids are dropped.
    void addIds(const LLSD& value, std::vector<LLUUID>& out)
    {
        if (value.isArray())
        {
            for (LLSD::array_const_iterator it = value.beginArray(); it != value.endArray(); ++it)
            {
                addIds(*it, out);
            }
            return;
        }
        LLUUID id;
        if (value.isUUID())
        {
            id = value.asUUID();
        }
        else if (value.isString() && LLUUID::validate(value.asString()))
        {
            id.set(value.asString());
        }
        if (id.notNull() && std::find(out.begin(), out.end(), id) == out.end())
        {
            out.push_back(id);   // (in file order, g then m then l: the colour fallback prefers the main group)
        }
    }

    struct Table
    {
        std::vector<GMDXSLMCFactions::Faction> mFactions;
        std::unordered_map<LLUUID, size_t> mByGroup;   // group -> index in mFactions

        Table()
        {
            const std::string path = gDirUtilp->getExpandedFilename(LL_PATH_APP_SETTINGS, FACTIONS_FILE);
            llifstream file(path.c_str());
            LLSD sd;
            if (!file.is_open() || LLSDSerialize::fromXML(sd, file) == LLSDParser::PARSE_FAILURE)
            {
                LL_WARNS("SLMCFactions") << "Could not read " << path << LL_ENDL;
                return;
            }
            const LLSD& factions = sd["factions"];
            if (!factions.isArray())
            {
                LL_WARNS("SLMCFactions") << "No faction list in " << path << LL_ENDL;
                return;
            }
            for (LLSD::array_const_iterator it = factions.beginArray(); it != factions.endArray(); ++it)
            {
                const LLSD& entry = *it;
                if (!entry.isMap())
                {
                    continue;
                }
                GMDXSLMCFactions::Faction faction;
                faction.mName = entry["name"].asString();
                faction.mShort = entry["short"].asString();
                for (const char* key : { "g", "m", "l" })
                {
                    if (entry.has(key))
                    {
                        addIds(entry[key], faction.mGroups);
                    }
                }
                if (faction.mGroups.empty())
                {
                    continue;
                }
                // (a group listed by two factions stays with the first, as Skooma's factionOf)
                for (const LLUUID& group : faction.mGroups)
                {
                    mByGroup.emplace(group, mFactions.size());
                }
                mFactions.push_back(std::move(faction));
            }
            LL_INFOS("SLMCFactions") << "Loaded " << mFactions.size() << " SLMC factions from " << path << LL_ENDL;
        }
    };

    const Table& table()
    {
        static const Table s_table;
        return s_table;
    }
}

const GMDXSLMCFactions::Faction* GMDXSLMCFactions::factionOf(const LLUUID& group)
{
    if (group.isNull())
    {
        return nullptr;
    }
    const Table& t = table();
    auto it = t.mByGroup.find(group);
    return it != t.mByGroup.end() ? &t.mFactions[it->second] : nullptr;
}
