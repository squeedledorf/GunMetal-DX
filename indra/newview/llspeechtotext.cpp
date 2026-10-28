/**
 * @file llspeechtotext.cpp
 * @brief Local speech-to-text (#1118). See llspeechtotext.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llspeechtotext.h"

#include "llapp.h"
#include "llcorehttputil.h"
#include "llcoros.h"
#include "lldir.h"
#include "lleventcoro.h"
#include "llfile.h"
#include "llvraudio.h"
#include "slupdateverify.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <thread>

#if LL_OPENAL
#include "AL/al.h"
#include "AL/alc.h"
#endif

#if LL_WHISPER
#include <whisper.h>   // (types only: every call goes through the run-time loaded module)
#endif

#if LL_LINUX
#include <cpuid.h>
#include <dlfcn.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif LL_WINDOWS
#include "llwin32headers.h"
#include <intrin.h>
#endif

namespace
{
// ---- Models ----
// OpenAI Whisper weights (MIT) in ggml format, 8-bit quantized (on CPU q8_0 is both smaller
// and faster than the f16 originals; q5_1 measured ~25% slower). Sizes / SHA-256 from the
// ggerganov/whisper.cpp Hugging Face repo at the pinned revision below.
const LLSpeechToText::Model kModels[] = {
    {"base.en", "ggml-base.en-q8_0.bin", 81781811ull,
     "a4d4a0768075e13cfd7e19df3ae2dbc4a68d37d36a7dad45e8410c9a34f8c87e", false},
    {"base", "ggml-base-q8_0.bin", 81768585ull,
     "c577b9a86e7e048a0b7eada054f4dd79a56bbfa911fbdacf900ac5b567cbb7d9", true},
    {"small.en", "ggml-small.en-q8_0.bin", 264477561ull,
     "67a179f608ea6114bd3fdb9060e762b588a3fb3bd00c4387971be4d177958067", false},
};
const LLSpeechToText::Model& kBaseEn = kModels[0];
const LLSpeechToText::Model& kBaseMulti = kModels[1];
const LLSpeechToText::Model& kSmallEn = kModels[2];

// small.en is ~3x the work of base.en and the encoder always covers the 30 s window, so its
// latency is a fixed ~0.9 s (8 threads) .. ~1.55 s (4 threads) on a Threadripper 7960X. It is
// the automatic choice from 24 hardware threads (6+ inference threads, ~1.2 s), and given up
// for the session when two clips in a row take longer than kSlowInferMs.
constexpr U32 kSmallMinHardwareThreads = 24;
std::atomic<int> sSlowStrikes{0};
#if LL_WHISPER
constexpr F64 kSlowInferMs = 1800.0;
// Whisper's initial prompt: at most this many tokens (the decoder's prompt budget is 223).
constexpr int kMaxPromptTokens = 120;
#endif

// Where the files are fetched from, in order: our own mirror (Forgejo release assets of
// sky_line/slipstream-3p), then the upstream repository at a pinned revision. The checksum,
// not the host, is what makes a file acceptable.
const char* const kModelUrlBases[] = {
    "https://git.slipstreamviewer.org/sky_line/slipstream-3p/releases/download/whisper-models-5359861/",
    "https://huggingface.co/ggerganov/whisper.cpp/resolve/5359861c739e955e79d9a303bcbc70fb988958b1/",
};

constexpr U64 kChunkBytes = 4u * 1024 * 1024;   // ranged requests: bounded memory, resumable
constexpr S32 kMaxChunkFailures = 4;

constexpr U32 kRate = 16000;                     // whisper's input rate
constexpr size_t kMaxSamples = kRate * 30;       // one clip: whisper's 30 s window
constexpr size_t kMinSamples = kRate + kRate / 10;   // shorter clips are padded with silence to 1.1 s

bool sDownloading = false;
F32  sDownloadProgress = 0.f;

std::string modelDir()
{
    return gDirUtilp->add(gDirUtilp->getOSUserAppDir(), "speech_models");
}

U64 fileSize(const std::string& path)
{
    llstat st;
    return LLFile::stat(path, &st) == 0 ? (U64)st.st_size : 0;
}

// GET one byte range. Returns the HTTP status; `body` receives the data on 206 (or on 200 when
// the server sent the whole file).
S32 getRange(const std::string& url, U64 first, U64 last, std::string& body)
{
    LLCore::HttpRequest::policy_t policy(LLCore::HttpRequest::DEFAULT_POLICY_ID);
    LLCoreHttpUtil::HttpCoroutineAdapter::ptr_t adapter(
        new LLCoreHttpUtil::HttpCoroutineAdapter("SpeechModelDownload", policy));
    LLCore::HttpRequest::ptr_t request(new LLCore::HttpRequest);
    LLCore::HttpOptions::ptr_t options(new LLCore::HttpOptions);
    options->setTimeout(60);
    options->setRetries(0);
    options->setFollowRedirects(true);
    LLCore::HttpHeaders::ptr_t headers(new LLCore::HttpHeaders);
    headers->append("Range", llformat("bytes=%llu-%llu", (unsigned long long)first, (unsigned long long)last));
    LLSD result = adapter->getRawAndSuspend(request, url, options, headers);
    const LLSD& http = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS];
    const LLCore::HttpStatus status = LLCoreHttpUtil::HttpCoroutineAdapter::getStatusFromLLSD(http);
    // An HTTP error (404 / 403 ...) is carried in the status' type; the results' status field holds
    // the generic error code then (1), which made the mirror's 404 look like a transient failure and
    // the download never fell back to the next host.
    const S32 code = status.isHttpStatus() ? (S32)status.getType()
                                           : http[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_STATUS].asInteger();
    if (status && (code == 206 || code == 200))
    {
        const LLSD::Binary& raw = result[LLCoreHttpUtil::HttpCoroutineAdapter::HTTP_RESULTS_RAW].asBinary();
        body.assign(raw.begin(), raw.end());
    }
    return code;
}

// SHA-256 of a big file off the main thread; the coroutine polls for completion.
std::string hashInThread(const std::string& path, unsigned long long& size)
{
    std::atomic<bool> done(false);
    std::string digest;
    unsigned long long bytes = 0;
    std::thread worker([&]()
    {
        digest = SLUpdateVerify::sha256File(path, &bytes);
        done = true;
    });
    while (!done && !LLApp::isExiting())
    {
        llcoro::suspendUntilTimeout(0.05f);
    }
    worker.join();
    size = bytes;
    return digest;
}

void downloadCoro(LLSpeechToText::Model m, LLSpeechToText::download_done_t done)
{
    const std::string dir = modelDir();
    LLFile::mkdir(dir);
    const std::string final_path = gDirUtilp->add(dir, m.file);
    const std::string part = final_path + ".part";
    auto finish = [&](bool ok)
    {
        sDownloading = false;
        if (!LLApp::isExiting() && done) done(ok);
    };

    U64 have = fileSize(part);
    if (have > m.bytes)
    {
        LLFile::remove(part);
        have = 0;
    }
    LL_INFOS("SpeechToText") << "Downloading speech model " << m.file << " (" << m.bytes << " bytes), resuming at "
                             << have << LL_ENDL;
    size_t url_index = 0;
    S32 failures = 0;
    while (have < m.bytes && !LLApp::isExiting())
    {
        const std::string url = std::string(kModelUrlBases[url_index]) + m.file;
        const U64 last = std::min<U64>(have + kChunkBytes, m.bytes) - 1;
        std::string body;
        const S32 code = getRange(url, have, last, body);
        if (LLApp::isExiting()) break;
        const bool whole = code == 200 && have == 0 && body.size() == m.bytes;   // the server ignored Range
        if (!whole && (code != 206 || body.size() != (size_t)(last - have + 1)))
        {
            LL_WARNS("SpeechToText") << "Model chunk " << have << "-" << last << " from " << url << " failed: HTTP "
                                     << code << ", " << body.size() << " bytes" << LL_ENDL;
            // Not on this host at all (or it ignores Range): the next one, from where we are.
            if ((code == 404 || code == 403 || code == 200) && url_index + 1 < LL_ARRAY_SIZE(kModelUrlBases))
            {
                ++url_index;
                failures = 0;
                continue;
            }
            if (++failures >= kMaxChunkFailures)
            {
                finish(false);
                return;
            }
            llcoro::suspendUntilTimeout(2.f * failures);
            continue;
        }
        failures = 0;
        {
            std::ofstream out(part, std::ios::binary | std::ios::app);
            out.write(body.data(), (std::streamsize)body.size());
            if (!out.flush())
            {
                LL_WARNS("SpeechToText") << "Cannot write " << part << " (disk full?)" << LL_ENDL;
                finish(false);
                return;
            }
        }
        have += body.size();
        sDownloadProgress = (F32)((F64)have / (F64)m.bytes);
    }
    if (have < m.bytes)
    {
        finish(false);   // exiting: resumes next time
        return;
    }
    unsigned long long size = 0;
    const std::string digest = hashInThread(part, size);
    if (LLApp::isExiting())
    {
        finish(false);
        return;
    }
    if (size != m.bytes || !SLUpdateVerify::sameDigest(digest, m.sha256))
    {
        LL_WARNS("SpeechToText") << "Downloaded speech model " << m.file << " does not match its checksum (sha256 "
                                 << digest << "); discarding it" << LL_ENDL;
        LLFile::remove(part);
        finish(false);
        return;
    }
    LLFile::remove(final_path);
    if (LLFile::rename(part, final_path) != 0)
    {
        LL_WARNS("SpeechToText") << "Cannot rename " << part << LL_ENDL;
        finish(false);
        return;
    }
    LL_INFOS("SpeechToText") << "Speech model " << m.file << " downloaded and verified" << LL_ENDL;
    finish(true);
}

// ---- Capture ----
#if LL_OPENAL
ALCdevice* sCapDev = nullptr;
#endif
std::vector<S16> sCapBuf;

#if LL_OPENAL
std::vector<std::string> alcNames(const ALCchar* list)
{
    std::vector<std::string> out;
    while (list && *list)
    {
        out.emplace_back(list);
        list += out.back().size() + 1;
    }
    return out;
}
#endif

// ---- Worker ----
struct Job
{
    U32 serial = 0;
    std::vector<S16> pcm;
    std::string language;
    const LLSpeechToText::Model* model = nullptr;
    std::vector<std::string> vocabulary;         // most important first
};

std::mutex sMutex;
std::condition_variable sCv;
std::deque<Job> sJobs;                       // guarded by sMutex
std::deque<LLSpeechToText::Result> sResults; // guarded by sMutex
bool sQuit = false;                          // guarded by sMutex
std::atomic<bool> sAbort{false};             // whisper's abort callback reads it
std::atomic<int> sPending{0};                // queued + running clips
std::thread sThread;                         // main thread starts / joins it
U32 sNextSerial = 1;                         // main thread
std::string sModulePaths[2];                 // where to look for the module (set before the worker starts)

#if LL_WHISPER
// The whisper.cpp module (cmake/Whisper.cmake: lib/libslwhisper.so, slwhisper.dll), loaded
// by the worker on its first clip and unloaded when it stops. Built for AVX2 and kept out
// of the viewer binary so its code never runs on a CPU without it (cpuRunsModule()).
struct WhisperApi
{
    void* lib = nullptr;
    decltype(&whisper_context_default_params) contextDefaultParams = nullptr;
    decltype(&whisper_init_from_file_with_params) initFromFile = nullptr;
    decltype(&whisper_free) freeContext = nullptr;
    decltype(&whisper_full_default_params) fullDefaultParams = nullptr;
    decltype(&whisper_full) full = nullptr;
    decltype(&whisper_full_n_segments) segments = nullptr;
    decltype(&whisper_full_get_segment_text) segmentText = nullptr;
    decltype(&whisper_full_get_segment_no_speech_prob) segmentNoSpeech = nullptr;
    decltype(&whisper_lang_id) langId = nullptr;
    decltype(&whisper_tokenize) tokenize = nullptr;
    decltype(&whisper_log_set) logSet = nullptr;
    decltype(&ggml_set_abort_callback) setAbortCallback = nullptr;
};

// Worker-owned.
WhisperApi wApi;
whisper_context* wCtx = nullptr;
std::string wCtxPath;
std::vector<std::string> wVerified;          // model files whose checksum this process checked

void* moduleSymbol(void* lib, const char* name)
{
#if LL_WINDOWS
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(lib), name));
#else
    return dlsym(lib, name);
#endif
}

template <typename T> bool bindSymbol(void* lib, T& fn, const char* name)
{
    fn = reinterpret_cast<T>(moduleSymbol(lib, name));
    if (!fn) LL_WARNS("SpeechToText") << "speech module: no " << name << LL_ENDL;
    return fn != nullptr;
}

void unloadModule()
{
    if (!wApi.lib) return;
#if LL_WINDOWS
    FreeLibrary(static_cast<HMODULE>(wApi.lib));
#else
    dlclose(wApi.lib);
#endif
    wApi = WhisperApi();
}

void whisperLog(enum ggml_log_level level, const char* text, void*);
void ggmlAbort(const char* message);

bool loadModule()
{
    if (wApi.lib) return true;
    for (const std::string& path : sModulePaths)
    {
        if (path.empty()) continue;
#if LL_WINDOWS
        wApi.lib = LoadLibraryW(ll_convert_string_to_wide(path).c_str());
#else
        wApi.lib = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        if (wApi.lib)
        {
            LL_INFOS("SpeechToText") << "speech module loaded: " << path << LL_ENDL;
            break;
        }
#if LL_WINDOWS
        LL_INFOS("SpeechToText") << "speech module not loaded from " << path << " (error " << GetLastError() << ")" << LL_ENDL;
#else
        const char* err = dlerror();
        LL_INFOS("SpeechToText") << "speech module not loaded from " << path << ": " << (err ? err : "?") << LL_ENDL;
#endif
    }
    if (!wApi.lib) return false;
    void* lib = wApi.lib;
    const bool ok = bindSymbol(lib, wApi.contextDefaultParams, "whisper_context_default_params") &&
                    bindSymbol(lib, wApi.initFromFile, "whisper_init_from_file_with_params") &&
                    bindSymbol(lib, wApi.freeContext, "whisper_free") &&
                    bindSymbol(lib, wApi.fullDefaultParams, "whisper_full_default_params") &&
                    bindSymbol(lib, wApi.full, "whisper_full") &&
                    bindSymbol(lib, wApi.segments, "whisper_full_n_segments") &&
                    bindSymbol(lib, wApi.segmentText, "whisper_full_get_segment_text") &&
                    bindSymbol(lib, wApi.segmentNoSpeech, "whisper_full_get_segment_no_speech_prob") &&
                    bindSymbol(lib, wApi.langId, "whisper_lang_id") &&
                    bindSymbol(lib, wApi.tokenize, "whisper_tokenize") &&
                    bindSymbol(lib, wApi.logSet, "whisper_log_set") &&
                    bindSymbol(lib, wApi.setAbortCallback, "ggml_set_abort_callback");
    if (!ok)
    {
        unloadModule();
        return false;
    }
    wApi.logSet(whisperLog, nullptr);
    wApi.setAbortCallback(ggmlAbort);
    return true;
}

void whisperLog(enum ggml_log_level level, const char* text, void*)
{
    std::string s(text ? text : "");
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
    if (s.empty()) return;
    if (level == GGML_LOG_LEVEL_ERROR || level == GGML_LOG_LEVEL_WARN)
        LL_WARNS("SpeechToText") << "whisper: " << s << LL_ENDL;
    else
        LL_DEBUGS("SpeechToText") << "whisper: " << s << LL_ENDL;
}

// ggml asserts call this, then abort(): make it the viewer's own crash with the message.
void ggmlAbort(const char* message)
{
    LL_ERRS("SpeechToText") << "ggml: " << (message ? message : "(no message)") << LL_ENDL;
}

bool whisperAbort(void*)
{
    return sAbort.load();
}

int inferenceThreads(const LLSpeechToText::Model& m)
{
    // The GPU and the main / render threads are busy with the headset: a quarter of the
    // hardware threads, 2..4 (4 measured ~0.45 s for base.en on a 7960X). small.en is ~3x the
    // work: 4..8 threads (8 measured ~0.9 s).
    const int hw = (int)std::max(1u, std::thread::hardware_concurrency());
    return &m == &kSmallEn ? llclamp(hw / 4, 4, 8) : llclamp(hw / 4, 2, 4);
}

std::string lowerAscii(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// Whisper's initial prompt from the vocabulary (most important first): "Glossary: a, b, c."
// with as many terms as fit kMaxPromptTokens, duplicates (any case) dropped. The prompt's end
// is what the decoder weighs most (and what survives truncation), so the most important term
// goes LAST. Returns "" when there is nothing; `tokens` gets the prompt's token count.
std::string buildPrompt(whisper_context* ctx, const std::vector<std::string>& vocabulary, int& tokens)
{
    tokens = 0;
    std::vector<std::string> kept;
    auto promptOf = [&kept]()
    {
        std::string p = "Glossary: ";
        for (size_t i = kept.size(); i-- > 0;)
        {
            p += kept[i];
            p += i > 0 ? ", " : ".";
        }
        return p;
    };
    auto count = [&](const std::string& p)
    {
        std::vector<whisper_token> t(kMaxPromptTokens * 4 + 64);
        const int n = wApi.tokenize(ctx, p.c_str(), t.data(), (int)t.size());
        return n < 0 ? (int)t.size() : n;   // (< 0: more than the buffer holds: far over the budget)
    };
    std::vector<std::string> seen;
    for (const std::string& term : vocabulary)
    {
        const std::string key = lowerAscii(term);
        if (term.empty() || std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        kept.push_back(term);
        if (count(promptOf()) > kMaxPromptTokens) kept.pop_back();
    }
    if (kept.empty()) return std::string();
    const std::string prompt = promptOf();
    tokens = count(prompt);
    return prompt;
}

// The transcript, cleaned: segments joined, "[BLANK_AUDIO]" / "[Music]" style markers and a
// lone "(...)" non-speech description removed, whitespace collapsed.
std::string cleanText(const std::string& raw)
{
    std::string out;
    out.reserve(raw.size());
    int bracket = 0;
    for (char c : raw)
    {
        if (c == '[') { ++bracket; continue; }
        if (c == ']' && bracket > 0) { --bracket; continue; }
        if (bracket > 0) continue;
        const bool ws = c == ' ' || c == '\t' || c == '\n' || c == '\r';
        if (ws)
        {
            if (!out.empty() && out.back() != ' ') out.push_back(' ');
        }
        else
        {
            out.push_back(c);
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() >= 2 && out.front() == '(' && out.back() == ')' && out.find(')') == out.size() - 1) out.clear();
    return out;
}

LLSpeechToText::Result runJob(Job& job)
{
    LLSpeechToText::Result r;
    r.serial = job.serial;
    r.audioSec = (F32)job.pcm.size() / (F32)kRate;
    const auto t0 = std::chrono::steady_clock::now();
    if (!loadModule())
    {
        r.error = "the speech library is missing from this installation";
        return r;
    }
    const LLSpeechToText::Model& m = *job.model;
    const std::string path = LLSpeechToText::modelPath(m);
    if (wCtxPath != path)
    {
        if (wCtx) wApi.freeContext(wCtx);
        wCtx = nullptr;
        wCtxPath.clear();
        if (std::find(wVerified.begin(), wVerified.end(), path) == wVerified.end())
        {
            unsigned long long size = 0;
            const std::string digest = SLUpdateVerify::sha256File(path, &size);
            if (size != m.bytes || !SLUpdateVerify::sameDigest(digest, m.sha256))
            {
                r.error = "the speech model on disk does not match its checksum";
                LL_WARNS("SpeechToText") << path << ": " << r.error << " (sha256 " << digest << ")" << LL_ENDL;
                LLFile::remove(path);   // (downloaded again on the next use, with consent)
                return r;
            }
            wVerified.push_back(path);
        }
        whisper_context_params cp = wApi.contextDefaultParams();
        cp.use_gpu = false;
        wCtx = wApi.initFromFile(path.c_str(), cp);
        if (!wCtx)
        {
            r.error = "the speech model could not be loaded";
            LL_WARNS("SpeechToText") << path << ": " << r.error << LL_ENDL;
            return r;
        }
        wCtxPath = path;
        LL_INFOS("SpeechToText") << "Speech model " << m.file << " loaded" << LL_ENDL;
    }

    std::vector<float> pcm(std::max(job.pcm.size(), kMinSamples), 0.f);
    for (size_t i = 0; i < job.pcm.size(); ++i) pcm[i] = (F32)job.pcm[i] / 32768.f;

    whisper_full_params p = wApi.fullDefaultParams(WHISPER_SAMPLING_GREEDY);
    p.n_threads = inferenceThreads(m);
    p.no_context = true;
    p.no_timestamps = true;
    p.single_segment = true;        // one clip <= 30 s
    p.print_special = false;
    p.print_progress = false;
    p.print_realtime = false;
    p.print_timestamps = false;
    p.suppress_blank = true;
    p.suppress_nst = true;          // no "(music)" style tokens
    p.temperature_inc = 0.f;        // no fallback re-decodes: bounded latency
    std::string lang = "en";
    if (m.multilingual)
    {
        lang = job.language;
        if (lang.empty() || lang == "auto" || wApi.langId(lang.c_str()) < 0) lang = "auto";
    }
    p.language = lang.c_str();
    p.abort_callback = whisperAbort;
    p.abort_callback_user_data = nullptr;
    // Vocabulary bias (names, places, the user's terms). Used by the first decode only (no
    // temperature fallback runs), applied even with no_context. Only the token count is logged.
    int prompt_tokens = 0;
    const std::string prompt = buildPrompt(wCtx, job.vocabulary, prompt_tokens);
    if (!prompt.empty()) p.initial_prompt = prompt.c_str();
    const auto t_full = std::chrono::steady_clock::now();
    if (wApi.full(wCtx, p, pcm.data(), (int)pcm.size()) != 0)
    {
        r.error = sAbort.load() ? "aborted" : "transcription failed";
        return r;
    }
    const F64 full_ms = std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - t_full).count();
    LL_INFOS("SpeechToText") << "whisper " << m.id << ": " << p.n_threads << " threads, prompt " << prompt_tokens
                             << " tokens, decode " << (S32)full_ms << " ms" << LL_ENDL;
    if (&m == &kSmallEn)
    {
        // Too slow for push-to-talk here (a loaded or slow CPU): base.en for the rest of the process.
        if (full_ms > kSlowInferMs) ++sSlowStrikes; else sSlowStrikes = 0;
    }
    std::string text;
    const int n = wApi.segments(wCtx);
    for (int i = 0; i < n; ++i)
    {
        // Whisper invents text for clips that hold no speech ("Thank you."): a segment the
        // model itself rates as most likely not speech is dropped.
        if (wApi.segmentNoSpeech(wCtx, i) > 0.8f) continue;
        const char* s = wApi.segmentText(wCtx, i);
        if (s) text += s;
    }
    r.text = cleanText(text);
    r.ok = true;
    r.inferMs = std::chrono::duration<F64, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return r;
}
#endif // LL_WHISPER

#if LL_WHISPER
// AVX2, FMA, F16C and BMI2, and the OS saves the YMM registers (XGETBV): what the module's
// code needs. CPUID directly (no compiler runtime helper: the Windows build is clang-cl
// linking with the MSVC runtime).
bool cpuRunsModule()
{
#if (defined(__x86_64__) || defined(_M_X64)) && (defined(__GNUC__) || defined(__clang__))
    unsigned int leaf1[4] = {0, 0, 0, 0}, leaf7[4] = {0, 0, 0, 0};
#if LL_WINDOWS
    int r[4];
    __cpuidex(r, 0, 0);
    if (r[0] < 7) return false;
    __cpuidex(r, 1, 0);
    for (int i = 0; i < 4; ++i) leaf1[i] = (unsigned int)r[i];
    __cpuidex(r, 7, 0);
    for (int i = 0; i < 4; ++i) leaf7[i] = (unsigned int)r[i];
#else
    if (__get_cpuid_max(0, nullptr) < 7) return false;
    __cpuid_count(1, 0, leaf1[0], leaf1[1], leaf1[2], leaf1[3]);
    __cpuid_count(7, 0, leaf7[0], leaf7[1], leaf7[2], leaf7[3]);
#endif
    const unsigned int ecx1 = leaf1[2], ebx7 = leaf7[1];
    const bool fma = (ecx1 >> 12) & 1, osxsave = (ecx1 >> 27) & 1, avx = (ecx1 >> 28) & 1, f16c = (ecx1 >> 29) & 1;
    const bool avx2 = (ebx7 >> 5) & 1, bmi2 = (ebx7 >> 8) & 1;
    if (!(fma && osxsave && avx && f16c && avx2 && bmi2)) return false;
    unsigned int xcr0_lo = 0, xcr0_hi = 0;
    __asm__ volatile("xgetbv" : "=a"(xcr0_lo), "=d"(xcr0_hi) : "c"(0));
    (void)xcr0_hi;
    return (xcr0_lo & 0x6) == 0x6;   // XMM and YMM state enabled
#else
    return false;
#endif
}
#endif // LL_WHISPER

void lowerThreadPriority()
{
    // Inference must not take CPU time from the frame. Linux: nice is per thread and the
    // threads ggml starts from this one inherit it.
#if LL_LINUX
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 10);
#elif LL_WINDOWS
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
#endif
}

void workerMain()
{
    LL_INFOS("SpeechToText") << "Speech-to-text worker started" << LL_ENDL;
    lowerThreadPriority();
    while (true)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(sMutex);
            sCv.wait(lock, [] { return sQuit || !sJobs.empty(); });
            if (sQuit) break;
            job = std::move(sJobs.front());
            sJobs.pop_front();
        }
        LLSpeechToText::Result r;
#if LL_WHISPER
        r = runJob(job);
#else
        r.serial = job.serial;
        r.error = "not available in this build";
#endif
        {
            std::lock_guard<std::mutex> lock(sMutex);
            if (!sQuit) sResults.push_back(std::move(r));
        }
        --sPending;
    }
#if LL_WHISPER
    if (wCtx) wApi.freeContext(wCtx);
    wCtx = nullptr;
    wCtxPath.clear();
    unloadModule();
#endif
    LL_INFOS("SpeechToText") << "Speech-to-text worker stopped, model freed" << LL_ENDL;
}
} // namespace

// ---- Models ----

const LLSpeechToText::Model& LLSpeechToText::modelFor(const std::string& language)
{
    if (!(language.empty() || language == "en")) return kBaseMulti;
    return std::thread::hardware_concurrency() >= kSmallMinHardwareThreads && sSlowStrikes.load() < 2 ? kSmallEn : kBaseEn;
}

const LLSpeechToText::Model* LLSpeechToText::fallbackFor(const Model& m)
{
    return &m == &kSmallEn ? &kBaseEn : nullptr;
}

std::string LLSpeechToText::modelPath(const Model& m)
{
    return gDirUtilp->add(modelDir(), m.file);
}

bool LLSpeechToText::modelPresent(const Model& m)
{
    return fileSize(modelPath(m)) == m.bytes;
}

void LLSpeechToText::downloadModel(const Model& m, download_done_t done)
{
    if (sDownloading) return;
    sDownloading = true;
    sDownloadProgress = 0.f;
    const Model copy = m;
    LLCoros::instance().launch("LLSpeechToText::download", [copy, done]() { downloadCoro(copy, done); });
}

bool LLSpeechToText::downloading()
{
    return sDownloading;
}

F32 LLSpeechToText::downloadProgress()
{
    return sDownloadProgress;
}

// ---- Capture ----

bool LLSpeechToText::startCapture(const std::string& preferred_name)
{
#if LL_OPENAL
    if (sCapDev) return true;
    std::vector<std::string> names = alcNames(alcGetString(nullptr, ALC_CAPTURE_DEVICE_SPECIFIER));
    names.erase(std::remove_if(names.begin(), names.end(),
                               [](const std::string& n) { return n.rfind("Monitor of ", 0) == 0; }),
                names.end());
    std::string chosen;
    if (!preferred_name.empty())
    {
        for (const std::string& n : names)
            if (n == preferred_name) chosen = n;
        if (chosen.empty())
        {
            for (const std::string& n : names)
            {
                if (LLVRAudio::matchHeadsetMic(n))
                {
                    chosen = n;
                    break;
                }
            }
        }
    }
    sCapDev = alcCaptureOpenDevice(chosen.empty() ? nullptr : chosen.c_str(), kRate, AL_FORMAT_MONO16, (ALCsizei)(kRate * 2));
    if (!sCapDev && !chosen.empty())
    {
        LL_WARNS("SpeechToText") << "Cannot open capture device '" << chosen << "'; trying the default" << LL_ENDL;
        chosen.clear();
        sCapDev = alcCaptureOpenDevice(nullptr, kRate, AL_FORMAT_MONO16, (ALCsizei)(kRate * 2));
    }
    if (!sCapDev)
    {
        LL_WARNS("SpeechToText") << "No capture device could be opened (" << names.size() << " listed)" << LL_ENDL;
        return false;
    }
    static bool s_logged_list = false;
    if (!s_logged_list)
    {
        s_logged_list = true;
        for (const std::string& n : names) LL_INFOS("SpeechToText") << "capture device: '" << n << "'" << LL_ENDL;
    }
    const ALCchar* opened = alcGetString(sCapDev, ALC_CAPTURE_DEVICE_SPECIFIER);
    LL_INFOS("SpeechToText") << "dictation capture on '" << (opened ? opened : "?") << "'"
                             << (chosen.empty() ? " (system default" : " (headset microphone")
                             << (preferred_name.empty() ? std::string(")") : ", headset mic '" + preferred_name + "')")
                             << LL_ENDL;
    sCapBuf.clear();
    sCapBuf.reserve(kRate * 8);
    alcCaptureStart(sCapDev);
    return true;
#else
    (void)preferred_name;
    return false;
#endif
}

void LLSpeechToText::pollCapture()
{
#if LL_OPENAL
    if (!sCapDev) return;
    ALCint avail = 0;
    alcGetIntegerv(sCapDev, ALC_CAPTURE_SAMPLES, 1, &avail);
    if (avail <= 0) return;
    const size_t room = kMaxSamples - std::min(kMaxSamples, sCapBuf.size());
    const size_t take = std::min((size_t)avail, room);
    if (take > 0)
    {
        const size_t old = sCapBuf.size();
        sCapBuf.resize(old + take);
        alcCaptureSamples(sCapDev, sCapBuf.data() + old, (ALCsizei)take);
    }
    if ((size_t)avail > take)   // over the clip limit: drained and dropped
    {
        std::vector<S16> drop((size_t)avail - take);
        alcCaptureSamples(sCapDev, drop.data(), (ALCsizei)drop.size());
    }
#endif
}

bool LLSpeechToText::capturing()
{
#if LL_OPENAL
    return sCapDev != nullptr;
#else
    return false;
#endif
}

F32 LLSpeechToText::captureSeconds()
{
    return (F32)sCapBuf.size() / (F32)kRate;
}

std::vector<S16> LLSpeechToText::stopCapture()
{
#if LL_OPENAL
    if (sCapDev)
    {
        pollCapture();
        alcCaptureStop(sCapDev);
        alcCaptureCloseDevice(sCapDev);
        sCapDev = nullptr;
    }
#endif
    std::vector<S16> out;
    out.swap(sCapBuf);
    return out;
}

bool LLSpeechToText::trimSilence(std::vector<S16>& pcm)
{
    constexpr size_t kFrame = kRate / 50;   // 20 ms
    const size_t frames = pcm.size() / kFrame;
    if (frames < 10) return false;          // < 200 ms
    std::vector<F32> rms(frames);
    for (size_t f = 0; f < frames; ++f)
    {
        F64 acc = 0.0;
        for (size_t i = 0; i < kFrame; ++i)
        {
            const F64 v = pcm[f * kFrame + i];
            acc += v * v;
        }
        rms[f] = (F32)std::sqrt(acc / (F64)kFrame);
    }
    std::vector<F32> sorted(rms);
    std::sort(sorted.begin(), sorted.end());
    const F32 floor_level = sorted[sorted.size() / 10];   // the quietest 10%: the noise floor
    const F32 peak = sorted.back();
    // Speech: 3x (~10 dB) over the floor and over ~-48 dBFS; the loudest frame must reach
    // ~-36 dBFS, else the clip is near-silent (nothing is sent for it).
    const F32 threshold = std::max(floor_level * 3.f, 130.f);
    if (peak < 520.f) return false;
    size_t first = frames, last = 0, voiced = 0;
    for (size_t f = 0; f < frames; ++f)
    {
        if (rms[f] > threshold)
        {
            first = std::min(first, f);
            last = f;
            ++voiced;
        }
    }
    if (voiced < 8) return false;           // < 160 ms of speech
    const size_t pad = 10;                  // keep 200 ms around the speech
    const size_t from = (first > pad ? first - pad : 0) * kFrame;
    const size_t to = std::min(frames, last + 1 + pad) * kFrame;
    pcm = std::vector<S16>(pcm.begin() + from, pcm.begin() + to);
    return true;
}

// ---- Transcription ----

bool LLSpeechToText::available()
{
#if LL_WHISPER
    // The module is built for AVX2 / FMA / F16C / BMI2 (every machine with a ray-tracing
    // GPU has them; the viewer itself only assumes SSE4.2).
    static const bool s_cpu = cpuRunsModule();
    return s_cpu;
#else
    return false;
#endif
}

U32 LLSpeechToText::transcribe(std::vector<S16>&& pcm, const std::string& language, const Model& model,
                               std::vector<std::string>&& vocabulary)
{
    if (!available()) return 0;
    if (!sThread.joinable())
    {
        // The packaged layout first (Linux: bin/ + lib/; Windows: beside the exe), then
        // beside the binary (running from the build tree).
        const std::string exe_dir = gDirUtilp->getExecutableDir();
#if LL_WINDOWS
        sModulePaths[0] = gDirUtilp->add(exe_dir, "slwhisper.dll");
        sModulePaths[1].clear();
#else
        sModulePaths[0] = gDirUtilp->add(gDirUtilp->add(exe_dir, ".."), "lib/libslwhisper.so");
        sModulePaths[1] = gDirUtilp->add(exe_dir, "libslwhisper.so");
#endif
        {
            std::lock_guard<std::mutex> lock(sMutex);
            sQuit = false;
        }
        sAbort = false;
        sThread = std::thread(workerMain);
    }
    Job job;
    job.serial = sNextSerial++;
    if (sNextSerial == 0) sNextSerial = 1;
    job.pcm = std::move(pcm);
    job.language = language;
    job.model = &model;
    job.vocabulary = std::move(vocabulary);
    const U32 serial = job.serial;
    ++sPending;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sJobs.push_back(std::move(job));
    }
    sCv.notify_one();
    return serial;
}

bool LLSpeechToText::pollResult(Result& out)
{
    if (!sThread.joinable()) return false;
    std::lock_guard<std::mutex> lock(sMutex);
    if (sResults.empty()) return false;
    out = std::move(sResults.front());
    sResults.pop_front();
    return true;
}

bool LLSpeechToText::busy()
{
    return sPending.load() > 0;
}

void LLSpeechToText::release()
{
    if (capturing()) stopCapture();   // (discarded)
    if (!sThread.joinable()) return;
    sAbort = true;
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sQuit = true;
        sJobs.clear();
    }
    sCv.notify_one();
    sThread.join();
    {
        std::lock_guard<std::mutex> lock(sMutex);
        sResults.clear();
        sQuit = false;
    }
    sAbort = false;
    sPending = 0;
}

void LLSpeechToText::shutdown()
{
    release();
}
