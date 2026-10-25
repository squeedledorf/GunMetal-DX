/**
 * @file gmdxreplayaudio.cpp
 * @brief GunMetal-DX instant replay: the viewer's own sound, Opus-encoded for the replay's audio track.
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

// <GMDX> INSTANT REPLAY AUDIO (see the header). No viewer precompiled header: plain std + OS APIs.

#include "gmdxreplayaudio.h"

#include "llwebrtc.h"   // the Opus encoder (libwebrtc carries it; llwebrtc exports it)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#else
#include <dlfcn.h>
#include <unistd.h>
#include <fstream>
#include <sstream>
#include <pulse/pulseaudio.h>   // types only: libpulse is opened at run time
#endif

namespace
{
constexpr uint32_t kRate = 48000;
constexpr uint32_t kFrame = 960;                    // 20 ms
constexpr uint32_t kCh = 2;
constexpr size_t kFifoMax = kRate * kCh;            // 1 s per source: older sound is dropped
constexpr double kLag = 0.04;                       // a frame is mixed this long after it ends (sound arriving late)
constexpr double kFill = 0.08;                      // s of sound each source queue holds (delivery comes in bursts)

double clockNow()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

#if !defined(_WIN32)
#define GMDX_PA_FUNCS(X) \
    X(pa_threaded_mainloop_new) X(pa_threaded_mainloop_free) X(pa_threaded_mainloop_start) \
    X(pa_threaded_mainloop_stop) X(pa_threaded_mainloop_lock) X(pa_threaded_mainloop_unlock) \
    X(pa_threaded_mainloop_get_api) X(pa_context_new) X(pa_context_unref) X(pa_context_connect) \
    X(pa_context_disconnect) X(pa_context_get_state) X(pa_context_errno) X(pa_context_set_state_callback) \
    X(pa_context_set_subscribe_callback) X(pa_context_subscribe) X(pa_context_get_sink_input_info_list) \
    X(pa_context_get_sink_input_info) X(pa_context_get_sink_info_by_index) X(pa_context_get_client_info) X(pa_operation_unref) \
    X(pa_proplist_gets) X(pa_strerror) X(pa_stream_new) X(pa_stream_unref) X(pa_stream_set_monitor_stream) \
    X(pa_stream_connect_record) X(pa_stream_disconnect) X(pa_stream_set_read_callback) \
    X(pa_stream_readable_size) X(pa_stream_peek) X(pa_stream_drop)

struct PaApi
{
#define X(f) decltype(&::f) f = nullptr;
    GMDX_PA_FUNCS(X)
#undef X
    bool load(std::string& err)
    {
        void* lib = dlopen("libpulse.so.0", RTLD_NOW | RTLD_LOCAL);   // kept loaded for the process's life
        if (!lib) { err = "libpulse isn't installed"; return false; }
#define X(f) if (!(f = reinterpret_cast<decltype(&::f)>(dlsym(lib, #f)))) { err = "libpulse lacks " #f; return false; }
        GMDX_PA_FUNCS(X)
#undef X
        return true;
    }
};

// pid is this process or one it started (media plugins, CEF helpers)
bool inOurTree(long pid)
{
    const long me = (long)getpid();
    for (int depth = 0; depth < 32 && pid > 1; ++depth)
    {
        if (pid == me) return true;
        std::ifstream f("/proc/" + std::to_string(pid) + "/stat");
        std::string line;
        std::getline(f, line);
        const size_t p = line.rfind(')');   // the name may hold spaces and parentheses
        if (p == std::string::npos || p + 2 >= line.size()) return false;
        std::istringstream is(line.substr(p + 2));
        char state = 0;
        long ppid = 0;
        if (!(is >> state >> ppid)) return false;
        pid = ppid;
    }
    return false;
}
#else
// The process-loopback activation parameters (audioclientactivationparams.h, Windows SDK 10.0.20348+),
// spelled out so older SDKs and MinGW build too. Same layout.
struct GmdxLoopbackParams { DWORD targetPid; DWORD mode; };            // mode 0 = include the process tree
struct GmdxActivationParams { DWORD type; GmdxLoopbackParams loopback; };   // type 1 = process loopback

class ActivateHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject
{
public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT hr = E_FAIL;
    IAudioClient* client = nullptr;

    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *ppv = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (riid == __uuidof(IAgileObject))
            *ppv = static_cast<IAgileObject*>(this);
        else { *ppv = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return (ULONG)InterlockedIncrement(&mRefs); }
    STDMETHODIMP_(ULONG) Release() override
    {
        const LONG r = InterlockedDecrement(&mRefs);
        if (!r) delete this;
        return (ULONG)r;
    }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override
    {
        HRESULT activated = E_FAIL;
        IUnknown* unk = nullptr;
        hr = op->GetActivateResult(&activated, &unk);
        if (SUCCEEDED(hr)) hr = activated;
        if (SUCCEEDED(hr) && unk) hr = unk->QueryInterface(__uuidof(IAudioClient), (void**)&client);
        if (unk) unk->Release();
        SetEvent(done);
        return S_OK;
    }

private:
    ~ActivateHandler()
    {
        if (client) client->Release();
        CloseHandle(done);
    }
    LONG mRefs = 1;
};
#endif
} // namespace

struct GMDXReplayAudio::Impl
{
    // sources -> mixer: interleaved stereo float, one queue per playback stream
    struct Fifo
    {
        std::deque<float> q;
        double pos = 0.0;       // read position inside the first frame (resampling)
        double avg = -1.0;      // smoothed fill, frames (< 0: none yet)
        double lastPush = 0.0;  // clock of the last delivery
        bool primed = false;    // has held kFill once since the source last went quiet
        bool ended = false;     // removed once drained
    };
    std::mutex fifoMutex;
    std::map<uint32_t, Fifo> fifos;

    // mixer -> replay buffer
    std::thread mixer;
    std::atomic<bool> run{false};
    void* enc = nullptr;   // Opus (llwebrtc::opusEncoderCreate)
    uint32_t preSkip = 0;
    mutable std::mutex ringMutex;
    std::deque<std::vector<uint8_t>> ring;
    uint64_t ringFirst = 0;         // frame number of ring.front()
    double t0 = 0.0;                // when frame 0 started
    std::atomic<uint32_t> keepSeconds{30};
    std::atomic<uint32_t> sources{0};

    std::mutex errorMutex;
    std::string error;
    bool reported = false;
    std::atomic<bool> failed{false};   // no capture: saves leave the audio track out

#if defined(_WIN32)
    std::thread capture;
#else
    PaApi pa;
    pa_threaded_mainloop* loop = nullptr;
    pa_context* ctx = nullptr;
    bool connected = false;   // reached READY once
    struct TapCtx { Impl* impl; uint32_t input; };
    struct Tap { pa_stream* stream = nullptr; uint32_t sink = 0; std::unique_ptr<TapCtx> ctx; };
    std::map<uint32_t, Tap> taps;   // playback stream (sink input) -> our monitor of it; mainloop thread only
    std::map<uint32_t, uint32_t> asked;   // sink lookups under way: input -> sink (one per input)
#endif

    void fail(const std::string& msg)
    {
        failed = true;
        std::lock_guard<std::mutex> lk(errorMutex);
        if (!reported && error.empty()) error = msg;
    }

    void push(uint32_t source, const float* s, size_t samples)
    {
        std::lock_guard<std::mutex> lk(fifoMutex);
        Fifo& f = fifos[source];
        f.ended = false;
        f.lastPush = clockNow();
        std::deque<float>& q = f.q;
        q.insert(q.end(), s, s + samples);
        if (q.size() > kFifoMax) q.erase(q.begin(), q.begin() + (std::ptrdiff_t)(q.size() - kFifoMax));
    }

    void endSource(uint32_t source)   // its last sound still gets mixed
    {
        std::lock_guard<std::mutex> lk(fifoMutex);
        auto it = fifos.find(source);
        if (it != fifos.end()) it->second.ended = true;
    }

    // Every 20 ms of the clock, one packet: whatever the sources delivered (silence where they didn't).
    // The timeline never breaks, so packet k always starts at t0 + k / 50.
    void mixLoop()
    {
        std::vector<float> mix(kFrame * kCh);
        std::vector<uint8_t> out(4000);
        for (uint64_t k = 0; run.load();)
        {
            const double wait = t0 + (k + 1) / 50.0 + kLag - clockNow();
            if (wait > 0.0)
            {
                std::this_thread::sleep_for(std::chrono::duration<double>(std::min(wait, 0.02)));
                continue;
            }
            std::fill(mix.begin(), mix.end(), 0.f);
            {
                std::lock_guard<std::mutex> lk(fifoMutex);
                for (auto it = fifos.begin(); it != fifos.end();)
                {
                    Fifo& f = it->second;
                    size_t have = f.q.size() / kCh;   // frames
                    if (!f.primed && (have >= kFill * kRate || f.ended)) f.primed = true;
                    if (f.primed && have > (kFill + 0.15) * kRate)
                    {
                        // a burst after a stall (delivery held up, then flushed): catch up at once
                        // rather than play everything after it late
                        const size_t drop = have - (size_t)(kFill * kRate);
                        f.q.erase(f.q.begin(), f.q.begin() + (std::ptrdiff_t)(drop * kCh));
                        have -= drop;
                        f.avg = (double)have;
                    }
                    if (f.primed)
                    {
                        // The sound card's clock and steady_clock drift apart: read a little more or
                        // less than a frame (up to 0.5 %, resampled) so the queue stays near kFill,
                        // instead of growing into latency or running dry into clicks.
                        f.avg = f.avg < 0.0 ? (double)have : f.avg + ((double)have - f.avg) * 0.02;   // ~1 s
                        const double off = (f.avg - kFill * kRate) / kRate;   // s
                        const double step = 1.0 + std::clamp(off * 0.25, -0.005, 0.005);
                        double pos = f.pos;
                        for (uint32_t i = 0; i < kFrame; ++i, pos += step)
                        {
                            const size_t a = (size_t)pos;
                            if (a + 1 >= have) break;   // ran dry: silence for the rest
                            const float w = (float)(pos - (double)a);
                            for (uint32_t c = 0; c < kCh; ++c)
                                mix[i * kCh + c] += f.q[a * kCh + c] * (1.f - w) + f.q[(a + 1) * kCh + c] * w;
                        }
                        const size_t used = std::min((size_t)pos, have);
                        f.pos = (size_t)pos < have ? pos - (double)(size_t)pos : 0.0;
                        f.q.erase(f.q.begin(), f.q.begin() + (std::ptrdiff_t)(used * kCh));
                        if (f.ended && f.q.size() < 2 * kCh) f.q.clear();   // the interpolation's last frame
                        if (f.q.empty() && clockNow() - f.lastPush > 0.25)   // gone quiet, not a late burst
                        {
                            f.primed = false;
                            f.pos = 0.0;
                            f.avg = -1.0;
                        }
                    }
                    it = f.q.empty() && f.ended ? fifos.erase(it) : std::next(it);
                }
            }
            for (float& v : mix) v = std::clamp(v, -1.f, 1.f);
            int bytes = llwebrtc::opusEncodeFloat(enc, mix.data(), (int)kFrame, out.data(), (int)out.size());
            if (bytes <= 0)
            {
                std::fill(mix.begin(), mix.end(), 0.f);
                bytes = std::max(0, llwebrtc::opusEncodeFloat(enc, mix.data(), (int)kFrame, out.data(), (int)out.size()));
            }
            {
                std::lock_guard<std::mutex> lk(ringMutex);
                ring.emplace_back(out.begin(), out.begin() + bytes);
                const size_t keep = (size_t)(keepSeconds.load() + 3) * 50;
                while (ring.size() > keep) { ring.pop_front(); ++ringFirst; }
            }
            ++k;
        }
    }

#if defined(_WIN32)
    void captureLoop()
    {
        const HRESULT coHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        typedef HRESULT(WINAPI * ActivateFn)(LPCWSTR, REFIID, PROPVARIANT*, IActivateAudioInterfaceCompletionHandler*,
                                             IActivateAudioInterfaceAsyncOperation**);
        HMODULE mm = LoadLibraryW(L"Mmdevapi.dll");
        ActivateFn activate = mm ? (ActivateFn)(void*)GetProcAddress(mm, "ActivateAudioInterfaceAsync") : nullptr;
        IAudioClient* client = nullptr;
        IAudioCaptureClient* cap = nullptr;
        HANDLE ready = nullptr;
        HRESULT hr = E_FAIL;
        if (activate)
        {
            GmdxActivationParams ap{1, {GetCurrentProcessId(), 0}};
            PROPVARIANT pv;
            PropVariantInit(&pv);
            pv.vt = VT_BLOB;
            pv.blob.cbSize = sizeof(ap);
            pv.blob.pBlobData = (BYTE*)&ap;
            ActivateHandler* h = new ActivateHandler;
            IActivateAudioInterfaceAsyncOperation* op = nullptr;
            hr = activate(L"VAD\\Process_Loopback", __uuidof(IAudioClient), &pv, h, &op);
            if (SUCCEEDED(hr))
            {
                // up to 5 s, but give up at once when stopped (stop() joins this thread on the frame)
                DWORD w = WAIT_TIMEOUT;
                for (int i = 0; i < 50 && run.load() && w == WAIT_TIMEOUT; ++i) w = WaitForSingleObject(h->done, 100);
                hr = w == WAIT_OBJECT_0 ? h->hr : HRESULT_FROM_WIN32(ERROR_TIMEOUT);
                if (SUCCEEDED(hr) && h->client) { client = h->client; client->AddRef(); }
                else if (SUCCEEDED(hr)) hr = E_NOINTERFACE;
            }
            if (op) op->Release();
            h->Release();
        }
        if (client)
        {
            WAVEFORMATEX wf{};
            wf.wFormatTag = WAVE_FORMAT_PCM;
            wf.nChannels = kCh;
            wf.nSamplesPerSec = kRate;
            wf.wBitsPerSample = 16;
            wf.nBlockAlign = kCh * 2;
            wf.nAvgBytesPerSec = kRate * wf.nBlockAlign;
            hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                    2000000 /* 200 ms */, 0, &wf, nullptr);
            if (SUCCEEDED(hr)) { ready = CreateEventW(nullptr, FALSE, FALSE, nullptr); hr = client->SetEventHandle(ready); }
            if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), (void**)&cap);
            if (SUCCEEDED(hr)) hr = client->Start();
        }
        if (!run.load())
        {
            // stopped while starting
        }
        else if (!activate || FAILED(hr) || !cap)
        {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "couldn't start (Windows error 0x%08lX); it needs Windows 10 version 2004 or later",
                          (unsigned long)hr);
            fail(buf);
        }
        else
        {
            sources = 1;
            std::vector<float> conv;
            while (run.load())
            {
                WaitForSingleObject(ready, 100);
                UINT32 n = 0;
                while (run.load() && SUCCEEDED(cap->GetNextPacketSize(&n)) && n)
                {
                    BYTE* data = nullptr;
                    UINT32 frames = 0;
                    DWORD flags = 0;
                    if (FAILED(cap->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
                    conv.assign((size_t)frames * kCh, 0.f);
                    if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT))
                    {
                        const int16_t* s = (const int16_t*)data;
                        for (size_t i = 0; i < conv.size(); ++i) conv[i] = s[i] * (1.f / 32768.f);
                    }
                    cap->ReleaseBuffer(frames);
                    push(0, conv.data(), conv.size());
                }
            }
            client->Stop();
        }
        if (cap) cap->Release();
        if (client) client->Release();
        if (ready) CloseHandle(ready);
        if (SUCCEEDED(coHr)) CoUninitialize();
    }
#else
    // ---- libpulse, all on its mainloop thread ----
    static void onRead(pa_stream* s, size_t, void* user)
    {
        TapCtx* t = (TapCtx*)user;
        PaApi& pa = t->impl->pa;
        while (pa.pa_stream_readable_size(s) > 0)
        {
            const void* data = nullptr;
            size_t bytes = 0;
            if (pa.pa_stream_peek(s, &data, &bytes) < 0 || !bytes) break;
            if (data) t->impl->push(t->input, (const float*)data, bytes / sizeof(float));   // null = a hole
            pa.pa_stream_drop(s);
        }
    }

    void untap(uint32_t input)
    {
        auto it = taps.find(input);
        if (it == taps.end()) return;
        pa.pa_stream_set_read_callback(it->second.stream, nullptr, nullptr);
        pa.pa_stream_disconnect(it->second.stream);
        pa.pa_stream_unref(it->second.stream);
        taps.erase(it);
        endSource(input);
        sources = (uint32_t)taps.size();
    }

    struct Pending { Impl* impl; uint32_t input; uint32_t sink; };

    static void onSink(pa_context*, const pa_sink_info* sink, int eol, void* user)
    {
        Pending* p = (Pending*)user;
        if (eol)   // called once more with eol after the info (or alone, on an error)
        {
            auto a = p->impl->asked.find(p->input);
            if (a != p->impl->asked.end() && a->second == p->sink) p->impl->asked.erase(a);
            delete p;
            return;
        }
        Impl& m = *p->impl;
        if (!sink || !m.run.load()) return;
        m.untap(p->input);
        static const pa_sample_spec spec{PA_SAMPLE_FLOAT32LE, kRate, (uint8_t)kCh};
        pa_stream* s = m.pa.pa_stream_new(m.ctx, "GunMetal-DX instant replay", &spec, nullptr);
        if (!s) return;
        Tap& tap = m.taps[p->input];
        tap.stream = s;
        tap.sink = sink->index;
        tap.ctx.reset(new TapCtx{&m, p->input});
        m.pa.pa_stream_set_monitor_stream(s, p->input);
        m.pa.pa_stream_set_read_callback(s, &Impl::onRead, tap.ctx.get());
        pa_buffer_attr attr;
        std::memset(&attr, 0xFF, sizeof(attr));   // server defaults (-1) ...
        attr.fragsize = kRate / 100 * kCh * sizeof(float);   // ... but 10 ms fragments
        if (m.pa.pa_stream_connect_record(s, sink->monitor_source_name, &attr,
                                          (pa_stream_flags_t)(PA_STREAM_DONT_MOVE | PA_STREAM_ADJUST_LATENCY)) < 0)
            m.untap(p->input);
        m.sources = (uint32_t)m.taps.size();
    }

    // ours (pid in this process tree): monitor it on the sink it plays to (the monitor must be that
    // sink's monitor source); someone else's: leave it (and drop a tap that was ours before)
    static void decide(pa_context* c, Impl& m, uint32_t input, uint32_t sink, const char* pid)
    {
        if (!pid || !inOurTree(std::atol(pid))) { m.untap(input); return; }
        // not linked yet (a change event follows when it is); a lookup of the invalid index would
        // answer with the default sink on PipeWire, and every later event would re-tap it
        if (sink == PA_INVALID_INDEX) return;
        auto t = m.taps.find(input);
        if (t != m.taps.end() && t->second.sink == sink) return;   // tapped meanwhile (lookups overlap)
        auto a = m.asked.find(input);
        if (a != m.asked.end() && a->second == sink) return;   // already being looked up
        m.asked[input] = sink;
        if (pa_operation* o = m.pa.pa_context_get_sink_info_by_index(c, sink, &Impl::onSink, new Pending{&m, input, sink}))
            m.pa.pa_operation_unref(o);
        else
            m.asked.erase(input);
    }

    static void onClient(pa_context* c, const pa_client_info* cl, int eol, void* user)
    {
        Pending* p = (Pending*)user;
        if (eol) { delete p; return; }
        if (cl && p->impl->run.load())
            decide(c, *p->impl, p->input, p->sink, p->impl->pa.pa_proplist_gets(cl->proplist, PA_PROP_APPLICATION_PROCESS_ID));
    }

    static void onInput(pa_context* c, const pa_sink_input_info* in, int eol, void* user)
    {
        Impl& m = *(Impl*)user;
        if (eol || !in || !m.run.load()) return;
        auto it = m.taps.find(in->index);
        if (it != m.taps.end() && it->second.sink == in->sink) return;   // already tapped where it plays
        // pulse clients put the pid on the stream; native PipeWire ones (OpenAL Soft) only on their client
        if (const char* pid = m.pa.pa_proplist_gets(in->proplist, PA_PROP_APPLICATION_PROCESS_ID))
            decide(c, m, in->index, in->sink, pid);
        else if (in->client != PA_INVALID_INDEX)
        {
            if (pa_operation* o = m.pa.pa_context_get_client_info(c, in->client, &Impl::onClient, new Pending{&m, in->index, in->sink}))
                m.pa.pa_operation_unref(o);
        }
    }

    static void onEvent(pa_context* c, pa_subscription_event_type_t t, uint32_t index, void* user)
    {
        Impl& m = *(Impl*)user;
        if ((t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK) != PA_SUBSCRIPTION_EVENT_SINK_INPUT) return;
        if ((t & PA_SUBSCRIPTION_EVENT_TYPE_MASK) == PA_SUBSCRIPTION_EVENT_REMOVE) { m.untap(index); return; }
        if (pa_operation* o = m.pa.pa_context_get_sink_input_info(c, index, &Impl::onInput, &m)) m.pa.pa_operation_unref(o);
    }

    static void onState(pa_context* c, void* user)
    {
        Impl& m = *(Impl*)user;
        switch (m.pa.pa_context_get_state(c))
        {
        case PA_CONTEXT_READY:
            m.connected = true;
            m.pa.pa_context_set_subscribe_callback(c, &Impl::onEvent, &m);
            if (pa_operation* o = m.pa.pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SINK_INPUT, nullptr, nullptr))
                m.pa.pa_operation_unref(o);
            if (pa_operation* o = m.pa.pa_context_get_sink_input_info_list(c, &Impl::onInput, &m)) m.pa.pa_operation_unref(o);
            break;
        case PA_CONTEXT_FAILED:
            if (m.run.load())
                m.fail(std::string(m.connected ? "lost the sound server: " : "no sound server: ")
                       + m.pa.pa_strerror(m.pa.pa_context_errno(c)));
            break;
        default:
            break;
        }
    }

    void startCapture()
    {
        std::string err;
        if (!pa.pa_context_new && !pa.load(err)) { fail(err); return; }
        loop = pa.pa_threaded_mainloop_new();
        if (!loop) { fail("couldn't start libpulse"); return; }
        ctx = pa.pa_context_new(pa.pa_threaded_mainloop_get_api(loop), "GunMetal-DX instant replay");
        if (!ctx) { fail("couldn't start libpulse"); return; }
        pa.pa_context_set_state_callback(ctx, &Impl::onState, this);
        if (pa.pa_context_connect(ctx, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0)
        {
            fail(std::string("no sound server: ") + pa.pa_strerror(pa.pa_context_errno(ctx)));
            return;
        }
        pa.pa_threaded_mainloop_start(loop);
    }

    void stopCapture()
    {
        if (loop)
        {
            pa.pa_threaded_mainloop_lock(loop);
            while (!taps.empty()) untap(taps.begin()->first);
            if (ctx) pa.pa_context_disconnect(ctx);
            pa.pa_threaded_mainloop_unlock(loop);
            pa.pa_threaded_mainloop_stop(loop);
        }
        if (ctx) pa.pa_context_unref(ctx);
        if (loop) pa.pa_threaded_mainloop_free(loop);
        ctx = nullptr;
        loop = nullptr;
    }
#endif
};

GMDXReplayAudio::GMDXReplayAudio() : d(new Impl) {}

GMDXReplayAudio::~GMDXReplayAudio()
{
    stop();
}

void GMDXReplayAudio::start()
{
    Impl& m = *d;
    if (m.run.load()) return;
    int lookahead = 0;
    m.enc = llwebrtc::opusEncoderCreate(kRate, kCh, 160000, &lookahead);
    if (!m.enc) { m.fail("the Opus encoder didn't start"); return; }
    m.preSkip = (uint32_t)std::max(0, lookahead);
    {
        std::lock_guard<std::mutex> lk(m.ringMutex);
        m.ring.clear();
        m.ringFirst = 0;
        m.t0 = clockNow();
    }
    m.failed = false;
    m.run = true;
    m.mixer = std::thread([&m]() { m.mixLoop(); });
#if defined(_WIN32)
    m.capture = std::thread([&m]() { m.captureLoop(); });
#else
    m.startCapture();
#endif
}

void GMDXReplayAudio::stop()
{
    Impl& m = *d;
    if (!m.run.exchange(false)) return;
#if defined(_WIN32)
    if (m.capture.joinable()) m.capture.join();
#else
    m.stopCapture();
#endif
    if (m.mixer.joinable()) m.mixer.join();
    llwebrtc::opusEncoderDestroy(m.enc);
    m.enc = nullptr;
    std::lock_guard<std::mutex> lk(m.fifoMutex);
    m.fifos.clear();
}

void GMDXReplayAudio::setKeepSeconds(uint32_t seconds)
{
    d->keepSeconds = seconds;
}

GMDXReplayAudioClip GMDXReplayAudio::clip(double nowSec, uint32_t seconds) const
{
    const Impl& m = *d;
    GMDXReplayAudioClip c;
    if (m.failed.load()) return c;
    std::lock_guard<std::mutex> lk(m.ringMutex);
    const double first = std::ceil((nowSec - seconds - 1.0 - m.t0) * 50.0);
    const uint64_t k = std::max<uint64_t>(m.ringFirst, (uint64_t)std::max(0.0, first));
    if (k >= m.ringFirst + m.ring.size()) return c;
    // packet k is mixed at t0 + (k + 1) / 50 + kLag from sound that sat kFill in its queue
    c.startSec = m.t0 + k / 50.0 + 0.02 + kLag - kFill;
    c.channels = kCh;
    c.preSkip = m.preSkip;
    c.packets.assign(m.ring.begin() + (std::ptrdiff_t)(k - m.ringFirst), m.ring.end());
    return c;
}

bool GMDXReplayAudio::takeError(std::string& message)
{
    std::lock_guard<std::mutex> lk(d->errorMutex);
    if (d->reported || d->error.empty()) return false;
    d->reported = true;
    message = d->error;
    return true;
}

std::string GMDXReplayAudio::describe() const
{
    const Impl& m = *d;
    size_t n = 0;
    {
        std::lock_guard<std::mutex> lk(m.ringMutex);
        n = m.ring.size();
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "audio: %u stream(s), %.1f s buffered", m.sources.load(), n / 50.0);
    return buf;
}
