/**
 * @file llvraudiopulse.cpp
 * @brief VR (#1122): the headset's output on Linux through PulseAudio / PipeWire (llvraudiopulse.h).
 *
 * libpulse (LGPL) is opened at run time (dlopen "libpulse.so.0"); its headers are only used for
 * the types (decltype of the functions), so nothing links against it.
 */

#include "llviewerprecompiledheaders.h"

#include "llvraudiopulse.h"

#if LL_LINUX && defined(__has_include)
#if __has_include(<pulse/pulseaudio.h>)
#define SL_VRAUDIO_PULSE 1
#endif
#endif

#if defined(SL_VRAUDIO_PULSE)

#include <pulse/pulseaudio.h>
#include <dlfcn.h>

#include <chrono>
#include <thread>
#include <vector>

namespace
{
// ---- libpulse, loaded once (the first VR session on Linux) ----
struct PulseApi
{
    bool tried = false;
    bool ok = false;
    decltype(&pa_mainloop_new) mainloop_new = nullptr;
    decltype(&pa_mainloop_free) mainloop_free = nullptr;
    decltype(&pa_mainloop_get_api) mainloop_get_api = nullptr;
    decltype(&pa_mainloop_iterate) mainloop_iterate = nullptr;
    decltype(&pa_context_new) context_new = nullptr;
    decltype(&pa_context_connect) context_connect = nullptr;
    decltype(&pa_context_get_state) context_get_state = nullptr;
    decltype(&pa_context_disconnect) context_disconnect = nullptr;
    decltype(&pa_context_unref) context_unref = nullptr;
    decltype(&pa_context_get_card_info_list) get_card_info_list = nullptr;
    decltype(&pa_context_get_sink_info_list) get_sink_info_list = nullptr;
    decltype(&pa_context_get_server_info) get_server_info = nullptr;
    decltype(&pa_context_set_card_profile_by_name) set_card_profile_by_name = nullptr;
    decltype(&pa_operation_get_state) operation_get_state = nullptr;
    decltype(&pa_operation_cancel) operation_cancel = nullptr;
    decltype(&pa_operation_unref) operation_unref = nullptr;
    decltype(&pa_proplist_gets) proplist_gets = nullptr;
};

PulseApi s_pa;

template <typename T> bool paSym(void* lib, T& fn, const char* name)
{
    fn = reinterpret_cast<T>(dlsym(lib, name));
    return fn != nullptr;
}

bool loadPulse()
{
    if (s_pa.tried) return s_pa.ok;
    s_pa.tried = true;
    // Kept loaded for the process' life (never dlclose'd).
    void* lib = dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL);
    if (!lib)
    {
        const char* err = dlerror();
        LL_INFOS("VRAudio") << "libpulse.so.0 not available (" << (err ? err : "?")
                            << "): the headset output is found by its device name only" << LL_ENDL;
        return false;
    }
    s_pa.ok = paSym(lib, s_pa.mainloop_new, "pa_mainloop_new") && paSym(lib, s_pa.mainloop_free, "pa_mainloop_free")
           && paSym(lib, s_pa.mainloop_get_api, "pa_mainloop_get_api") && paSym(lib, s_pa.mainloop_iterate, "pa_mainloop_iterate")
           && paSym(lib, s_pa.context_new, "pa_context_new") && paSym(lib, s_pa.context_connect, "pa_context_connect")
           && paSym(lib, s_pa.context_get_state, "pa_context_get_state")
           && paSym(lib, s_pa.context_disconnect, "pa_context_disconnect") && paSym(lib, s_pa.context_unref, "pa_context_unref")
           && paSym(lib, s_pa.get_card_info_list, "pa_context_get_card_info_list")
           && paSym(lib, s_pa.get_sink_info_list, "pa_context_get_sink_info_list")
           && paSym(lib, s_pa.get_server_info, "pa_context_get_server_info")
           && paSym(lib, s_pa.set_card_profile_by_name, "pa_context_set_card_profile_by_name")
           && paSym(lib, s_pa.operation_get_state, "pa_operation_get_state")
           && paSym(lib, s_pa.operation_cancel, "pa_operation_cancel") && paSym(lib, s_pa.operation_unref, "pa_operation_unref")
           && paSym(lib, s_pa.proplist_gets, "pa_proplist_gets");
    if (!s_pa.ok)
        LL_WARNS("VRAudio") << "libpulse.so.0 lacks an entry point: the headset output is found by its device name only" << LL_ENDL;
    return s_pa.ok;
}

typedef std::chrono::steady_clock Clock;
constexpr int kDeadlineMs = 1500;   // per call (connect + queries + a profile switch)

// ---- Snapshots of what the server reports (copied out of the callbacks) ----
struct ProfileSnap
{
    std::string name;
    std::string desc;
    U32 priority = 0;
    U32 n_sources = 0;
    bool available = true;
};

struct PortSnap
{
    std::string name;
    std::string desc;
    std::string product;   // the port's "device.product.name" (the EDID monitor name on HDMI / DP)
    bool output = false;
    bool unavailable = false;
    std::vector<std::string> profiles;
};

struct CardSnap
{
    U32 index = 0;
    std::string name;
    std::string desc;
    std::string active;
    std::vector<ProfileSnap> profiles;
    std::vector<PortSnap> ports;
};

struct SinkSnap
{
    U32 card = PA_INVALID_INDEX;
    std::string name;
    std::string desc;
    std::string active_port;
    std::vector<std::string> ports;
};

std::string str(const char* s)
{
    return s ? std::string(s) : std::string();
}

void onCard(pa_context*, const pa_card_info* i, int eol, void* userdata)
{
    if (eol || !i) return;
    std::vector<CardSnap>& out = *static_cast<std::vector<CardSnap>*>(userdata);
    CardSnap c;
    c.index = i->index;
    c.name = str(i->name);
    c.desc = str(s_pa.proplist_gets(i->proplist, PA_PROP_DEVICE_DESCRIPTION));
    if (i->active_profile2) c.active = str(i->active_profile2->name);
    for (U32 p = 0; p < i->n_profiles && i->profiles2; ++p)
    {
        const pa_card_profile_info2* pi = i->profiles2[p];
        if (!pi) continue;
        ProfileSnap ps;
        ps.name = str(pi->name);
        ps.desc = str(pi->description);
        ps.priority = pi->priority;
        ps.n_sources = pi->n_sources;
        ps.available = pi->available != 0;
        c.profiles.push_back(ps);
    }
    for (U32 p = 0; p < i->n_ports && i->ports; ++p)
    {
        const pa_card_port_info* po = i->ports[p];
        if (!po) continue;
        PortSnap ps;
        ps.name = str(po->name);
        ps.desc = str(po->description);
        ps.product = po->proplist ? str(s_pa.proplist_gets(po->proplist, "device.product.name")) : std::string();
        ps.output = (po->direction & PA_DIRECTION_OUTPUT) != 0;
        ps.unavailable = po->available == PA_PORT_AVAILABLE_NO;
        for (U32 q = 0; q < po->n_profiles && po->profiles2; ++q)
            if (po->profiles2[q]) ps.profiles.push_back(str(po->profiles2[q]->name));
        c.ports.push_back(ps);
    }
    out.push_back(c);
}

void onSink(pa_context*, const pa_sink_info* i, int eol, void* userdata)
{
    if (eol || !i) return;
    std::vector<SinkSnap>& out = *static_cast<std::vector<SinkSnap>*>(userdata);
    SinkSnap s;
    s.card = i->card;
    s.name = str(i->name);
    s.desc = str(i->description);
    if (i->active_port) s.active_port = str(i->active_port->name);
    for (U32 p = 0; p < i->n_ports && i->ports; ++p)
        if (i->ports[p]) s.ports.push_back(str(i->ports[p]->name));
    out.push_back(s);
}

void onServer(pa_context*, const pa_server_info* i, void* userdata)
{
    if (i) *static_cast<std::string*>(userdata) = str(i->default_sink_name);
}

void onSuccess(pa_context*, int success, void* userdata)
{
    *static_cast<int*>(userdata) = success ? 1 : 0;
}

// One connection to the server, every step bounded by the deadline.
class PulseConn
{
public:
    PulseConn() : mDeadline(Clock::now() + std::chrono::milliseconds(kDeadlineMs))
    {
        mLoop = s_pa.mainloop_new();
        if (!mLoop) return;
        mContext = s_pa.context_new(s_pa.mainloop_get_api(mLoop), "Slipstream VR audio");
        if (!mContext) return;
        if (s_pa.context_connect(mContext, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) return;
        while (true)
        {
            const pa_context_state_t st = s_pa.context_get_state(mContext);
            if (st == PA_CONTEXT_READY)
            {
                mReady = true;
                return;
            }
            if (st == PA_CONTEXT_FAILED || st == PA_CONTEXT_TERMINATED || !step()) return;
        }
    }

    ~PulseConn()
    {
        if (mContext)
        {
            s_pa.context_disconnect(mContext);
            s_pa.context_unref(mContext);
        }
        if (mLoop) s_pa.mainloop_free(mLoop);
    }

    bool ready() const { return mReady; }

    // Runs an operation to its end; false = it failed to start or the deadline passed.
    bool run(pa_operation* op)
    {
        if (!op) return false;
        while (s_pa.operation_get_state(op) == PA_OPERATION_RUNNING)
        {
            if (!step())
            {
                s_pa.operation_cancel(op);
                s_pa.operation_unref(op);
                return false;
            }
        }
        s_pa.operation_unref(op);
        return true;
    }

    bool cards(std::vector<CardSnap>& out)
    {
        out.clear();
        return mReady && run(s_pa.get_card_info_list(mContext, onCard, &out));
    }

    bool sinks(std::vector<SinkSnap>& out)
    {
        out.clear();
        return mReady && run(s_pa.get_sink_info_list(mContext, onSink, &out));
    }

    std::string defaultSink()
    {
        std::string name;
        if (mReady) run(s_pa.get_server_info(mContext, onServer, &name));
        return name;
    }

    bool setProfile(const std::string& card, const std::string& profile)
    {
        int ok = -1;
        if (!mReady || !run(s_pa.set_card_profile_by_name(mContext, card.c_str(), profile.c_str(), onSuccess, &ok))) return false;
        return ok == 1;
    }

    // Waits a little (for a sink to appear); false = the deadline passed.
    bool pause(int ms)
    {
        const Clock::time_point until = Clock::now() + std::chrono::milliseconds(ms);
        while (Clock::now() < until)
            if (!step()) return false;
        return true;
    }

private:
    // One non-blocking pass of the loop, then a 1 ms nap; false = the deadline passed.
    bool step()
    {
        if (Clock::now() >= mDeadline) return false;
        s_pa.mainloop_iterate(mLoop, 0, nullptr);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return true;
    }

    Clock::time_point mDeadline;
    pa_mainloop* mLoop = nullptr;
    pa_context* mContext = nullptr;
    bool mReady = false;
};

const SinkSnap* sinkFor(const std::vector<SinkSnap>& sinks, U32 card, const std::string& port)
{
    for (const SinkSnap& s : sinks)
        if (s.card == card && s.active_port == port) return &s;
    for (const SinkSnap& s : sinks)
        if (s.card == card)
            for (const std::string& p : s.ports)
                if (p == port) return &s;
    return nullptr;
}

bool hasProfile(const PortSnap& port, const std::string& profile)
{
    for (const std::string& p : port.profiles)
        if (p == profile) return true;
    return false;
}

// The profile to switch to for `port`: available first, then keeping the card's inputs (a
// headset card with a mic keeps it), then a stereo one (not surround), then the server's priority.
const ProfileSnap* pickProfile(const CardSnap& card, const PortSnap& port)
{
    U32 active_sources = 0;
    for (const ProfileSnap& p : card.profiles)
        if (p.name == card.active) active_sources = p.n_sources;
    const ProfileSnap* best = nullptr;
    S64 best_score = -1;
    for (const ProfileSnap& p : card.profiles)
    {
        if (p.name == "off" || !hasProfile(port, p.name)) continue;
        const bool stereo = p.name.find("stereo") != std::string::npos && p.name.find("surround") == std::string::npos;
        const S64 score = (p.available ? (S64)1 << 40 : 0) + (p.n_sources >= active_sources ? (S64)1 << 39 : 0)
                        + (stereo ? (S64)1 << 38 : 0) + (S64)p.priority;
        if (score > best_score)
        {
            best_score = score;
            best = &p;
        }
    }
    return best;
}

double msSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}
} // namespace

bool LLVRAudioPulse::findHeadsetOutput(match_fn_t match, Headset& out)
{
    out = Headset();
    if (!match || !loadPulse()) return false;
    const Clock::time_point t0 = Clock::now();
    PulseConn conn;
    if (!conn.ready())
    {
        LL_INFOS("VRAudio") << "PulseAudio / PipeWire server not reachable (" << msSince(t0)
                            << " ms): the headset output is found by its device name only" << LL_ENDL;
        return false;
    }
    std::vector<CardSnap> cards;
    if (!conn.cards(cards))
    {
        LL_WARNS("VRAudio") << "PulseAudio: listing the sound cards failed / timed out (" << msSince(t0) << " ms)" << LL_ENDL;
        return false;
    }
    const std::string default_sink = conn.defaultSink();
    LL_INFOS("VRAudio") << "PulseAudio: " << cards.size() << " sound cards; default sink '" << default_sink << "'" << LL_ENDL;

    // The headset's port: its EDID product name first (the Index's DP audio), then its description.
    const CardSnap* hit_card = nullptr;
    const PortSnap* hit_port = nullptr;
    const char* hit_rule = nullptr;
    std::string hit_name;
    for (int pass = 0; pass < 2 && !hit_port; ++pass)
    {
        for (const CardSnap& c : cards)
        {
            for (const PortSnap& p : c.ports)
            {
                if (!p.output) continue;
                const std::string& name = pass == 0 ? p.product : p.desc;
                const char* rule = name.empty() ? nullptr : match(name);
                // (Every output port with a product name is logged once, matched or not.)
                if (pass == 0 && !p.product.empty())
                    LL_INFOS("VRAudio") << "  card '" << c.name << "' port '" << p.name << "' (" << p.desc << "): product '"
                                        << p.product << "'" << (p.unavailable ? ", unplugged" : "")
                                        << (rule ? std::string(" <- headset: ") + rule : std::string()) << LL_ENDL;
                if (rule && !hit_port)
                {
                    hit_card = &c;
                    hit_port = &p;
                    hit_rule = rule;
                    hit_name = name;
                }
            }
        }
    }
    if (!hit_port)
    {
        LL_INFOS("VRAudio") << "PulseAudio: no sound card port names a headset (" << msSince(t0)
                            << " ms); falling back to the device names" << LL_ENDL;
        return false;
    }
    out.card = hit_card->name;
    out.card_desc = hit_card->desc;
    out.port = hit_port->name;
    out.matched = hit_name;
    out.rule = hit_rule;
    LL_INFOS("VRAudio") << "PulseAudio: headset output = card '" << out.card << "' (" << out.card_desc << ") port '" << out.port
                        << "' ('" << out.matched << "', " << out.rule << "); card profile '" << hit_card->active << "'"
                        << (hit_port->unavailable ? " (the port reports unplugged: trying anyway)" : "") << LL_ENDL;

    if (!hasProfile(*hit_port, hit_card->active))
    {
        const ProfileSnap* prof = pickProfile(*hit_card, *hit_port);
        if (!prof)
        {
            LL_WARNS("VRAudio") << "PulseAudio: no profile of '" << out.card << "' has port '" << out.port
                                << "'; falling back to the device names" << LL_ENDL;
            out = Headset();
            return false;
        }
        LL_INFOS("VRAudio") << "PulseAudio: the active profile lacks the headset port: switching card '" << out.card << "' from '"
                            << hit_card->active << "' to '" << prof->name << "' (" << prof->desc << "); restored when the VR session ends"
                            << LL_ENDL;
        if (!conn.setProfile(out.card, prof->name))
        {
            LL_WARNS("VRAudio") << "PulseAudio: switching the profile failed / timed out (" << msSince(t0)
                                << " ms); falling back to the device names" << LL_ENDL;
            out = Headset();
            return false;
        }
        out.profile_before = hit_card->active;
        out.profile_set = prof->name;
    }

    // The sink (it can take a moment to appear after a profile switch).
    const U32 card_index = hit_card->index;
    std::vector<SinkSnap> sinks;
    for (int attempt = 0; attempt < 8; ++attempt)
    {
        if (conn.sinks(sinks))
        {
            if (const SinkSnap* s = sinkFor(sinks, card_index, out.port))
            {
                out.sink = s->name;
                out.sink_desc = s->desc;
                break;
            }
        }
        if (!conn.pause(60)) break;
    }
    if (out.sink.empty())
        LL_INFOS("VRAudio") << "PulseAudio: the headset sink is not there yet (" << msSince(t0) << " ms); looking again later" << LL_ENDL;
    else
        LL_INFOS("VRAudio") << "PulseAudio: headset sink '" << out.sink << "' = '" << out.sink_desc << "' (" << msSince(t0) << " ms)"
                            << LL_ENDL;
    return true;
}

bool LLVRAudioPulse::refreshSink(Headset& h)
{
    if (h.card.empty() || !loadPulse()) return false;
    PulseConn conn;
    std::vector<CardSnap> cards;
    std::vector<SinkSnap> sinks;
    if (!conn.cards(cards) || !conn.sinks(sinks)) return false;
    for (const CardSnap& c : cards)
    {
        if (c.name != h.card) continue;
        if (const SinkSnap* s = sinkFor(sinks, c.index, h.port))
        {
            h.sink = s->name;
            h.sink_desc = s->desc;
            LL_INFOS("VRAudio") << "PulseAudio: headset sink '" << h.sink << "' = '" << h.sink_desc << "'" << LL_ENDL;
            return true;
        }
    }
    return false;
}

void LLVRAudioPulse::restore(const Headset& h)
{
    if (h.card.empty() || h.profile_set.empty() || !loadPulse()) return;
    const Clock::time_point t0 = Clock::now();
    PulseConn conn;
    std::vector<CardSnap> cards;
    if (!conn.cards(cards))
    {
        LL_WARNS("VRAudio") << "PulseAudio: could not reach the server to put card '" << h.card << "' back to '" << h.profile_before
                            << "' (" << msSince(t0) << " ms)" << LL_ENDL;
        return;
    }
    for (const CardSnap& c : cards)
    {
        if (c.name != h.card) continue;
        if (c.active != h.profile_set)
        {
            LL_INFOS("VRAudio") << "PulseAudio: card '" << h.card << "' is on '" << c.active << "' now (not the '" << h.profile_set
                                << "' we set): left as it is" << LL_ENDL;
            return;
        }
        const bool ok = conn.setProfile(h.card, h.profile_before);
        LL_INFOS("VRAudio") << "PulseAudio: card '" << h.card << "' back to profile '" << h.profile_before << "'"
                            << (ok ? "" : " FAILED") << " (" << msSince(t0) << " ms)" << LL_ENDL;
        return;
    }
    LL_INFOS("VRAudio") << "PulseAudio: card '" << h.card << "' is gone: nothing to put back" << LL_ENDL;
}

#else // !defined(SL_VRAUDIO_PULSE)

bool LLVRAudioPulse::findHeadsetOutput(match_fn_t, Headset& out)
{
    out = Headset();
    return false;
}

bool LLVRAudioPulse::refreshSink(Headset&)
{
    return false;
}

void LLVRAudioPulse::restore(const Headset&)
{
}

#endif // SL_VRAUDIO_PULSE
