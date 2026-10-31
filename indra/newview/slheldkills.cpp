/**
 * @file slheldkills.cpp
 * @brief Freeze World (#1172): the kills held until the thaw (see slheldkills.h)
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * (LGPL v2.1, see slheldkills.h)
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "slheldkills.h"

bool SLHeldKills::packetAfter(U32 a, U32 b)
{
    const U32 gap = (a - b) % PACKET_ID_RANGE;
    return gap != 0 && gap < PACKET_ID_RANGE / 2;
}

bool SLHeldKills::voids(const SLKillKey& kill, const SLObjectUpdateStamp& update)
{
    return !update.from_cache && update.accepted
           && update.ip == kill.ip && update.port == kill.port
           && packetAfter(update.packet, kill.packet);
}

bool SLHeldKills::hold(const LLUUID& id, const SLKillKey& key, F64 t_first, const LLUUID& wearer, size_t cap)
{
    auto it = mHeld.find(id);
    if (it != mHeld.end())
    {
        SLKillKey& held = it->second.key;
        const bool same_key = held.ip == key.ip && held.port == key.port && held.local_id == key.local_id;
        if (!same_key || packetAfter(key.packet, held.packet))
        {
            held = key;
        }
        return true;
    }
    if (mHeld.size() >= cap) return false;
    Held& h = mHeld[id];
    h.key = key;
    h.t_first = h.wait_from = t_first;
    h.wearer = wearer;
    return true;
}

bool SLHeldKills::onUpdate(const LLUUID& id, const SLObjectUpdateStamp& update, F64 t)
{
    auto it = mHeld.find(id);
    if (it == mHeld.end() || !voids(it->second.key, update)) return false;
    mHeld.erase(it);
    for (auto& kv : mHeld)
    {
        if (kv.second.wearer == id)
        {
            kv.second.with_wearer = false;
            kv.second.wait_from = t;
        }
    }
    return true;
}

SLHeldKills::list_t SLHeldKills::takeDetaches(F64 t, F64 wait_s)
{
    list_t detached;
    for (auto& kv : mHeld)
    {
        Held& h = kv.second;
        if (h.wearer.isNull() || h.with_wearer) continue;
        if (mHeld.count(h.wearer)) h.with_wearer = true;
        else if (t - h.wait_from >= wait_s) detached.push_back(kv);
    }
    for (const auto& d : detached)
    {
        mHeld.erase(d.first);
    }
    return detached;
}

SLHeldKills::list_t SLHeldKills::takeAll()
{
    list_t all(mHeld.begin(), mHeld.end());
    mHeld.clear();
    return all;
}

void SLHeldKills::eraseIf(const std::function<bool(const LLUUID&)>& pred)
{
    for (auto it = mHeld.begin(); it != mHeld.end();)
    {
        it = pred(it->first) ? mHeld.erase(it) : std::next(it);
    }
}
