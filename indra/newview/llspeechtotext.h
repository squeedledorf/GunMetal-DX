/**
 * @file llspeechtotext.h
 * @brief Local speech-to-text (#1118, VR dictation): microphone capture, the speech model
 *        (download on consent, checksum) and transcription on a worker thread. No UI here:
 *        LLVRDictation (llvrdictation.h) is the VR front end; a desktop one can use the same.
 *
 * Engine: whisper.cpp (MIT, built from source by cmake/Whisper.cmake into the module
 * lib/libslwhisper.so / slwhisper.dll), CPU only -- the GPU renders the headset. The module
 * is built for AVX2 and loaded by the worker on its first clip (never linked: the viewer
 * itself only assumes SSE4.2); available() checks the CPU first.
 * Models (OpenAI Whisper, MIT, ggml format, 8-bit quantized):
 *  - "base.en" (ggml-base.en-q8_0.bin, 82 MB): English, the default;
 *  - "base"    (ggml-base-q8_0.bin, 82 MB): multilingual (99 languages).
 *  - "small.en" (ggml-small.en-q8_0.bin, 264 MB): English, chosen automatically on CPUs with
 *    >= 24 hardware threads (and dropped for the session if it measures slower than 1.8 s).
 * Measured (#1118, Threadripper 7960X, AVX2 build, model loaded; latency is nearly independent of
 * clip length, the encoder always covers Whisper's 30 s window): base.en 4 threads ~0.45 s;
 * small.en 4 threads ~1.55 s, 6 threads ~1.2 s, 8 threads ~0.9 s; ~250 MB RAM (base) while loaded.
 *
 * Privacy: audio is captured only between startCapture() and stopCapture() (push-to-talk),
 * stays in memory, and is never written to disk or sent anywhere; the only network access
 * is the model download the user agreed to.
 *
 * Nothing here runs or allocates until first used: no thread, no device, no module, no
 * model. The worker thread, the module and the model live until release() (the VR session
 * ended) or shutdown().
 *
 * Threading: every function is main-thread only; the worker is internal.
 */
#pragma once

#include "stdtypes.h"

#include <functional>
#include <string>
#include <vector>

class LLSpeechToText
{
public:
    // ---- The speech model ----
    struct Model
    {
        const char* id;          // "base.en" / "base" / "small.en"
        const char* file;        // file name (download URL leaf and on disk)
        U64         bytes;
        const char* sha256;
        bool        multilingual;
    };
    // The model for a dictation language: "en" (or empty) -> English, "small.en" (264 MB, more
    // accurate on names and rare words) on a CPU fast enough for it (see fastCpu()), else "base.en";
    // anything else ("auto", "de", "ja", ...) -> the multilingual "base".
    static const Model& modelFor(const std::string& language);
    // A model to use while the preferred one is not on disk yet (small.en -> base.en); null = none.
    static const Model* fallbackFor(const Model& m);
    // <user app dir>/speech_models/<file> (not the cache: clearing the cache is a routine support step).
    static std::string modelPath(const Model& m);
    // On disk with the right size (the checksum was checked when it was downloaded, and again
    // when the worker first loads it).
    static bool modelPresent(const Model& m);

    // Download (ranged requests on a coroutine, resumable, then SHA-256 checked off the main
    // thread). done(true) = present and verified. One download at a time.
    typedef std::function<void(bool ok)> download_done_t;
    static void downloadModel(const Model& m, download_done_t done);
    static bool downloading();
    static F32 downloadProgress();   // 0..1

    // ---- Capture (push-to-talk) ----
    // Opens a capture device (16 kHz mono 16-bit; OpenAL Soft resamples) and starts it. The
    // device: an exact or headset-rule match of preferred_name (LLVRAudio::headsetMicName()),
    // else the system default. Independent of voice chat: PipeWire / PulseAudio and WASAPI
    // shared mode give each client its own stream. False = no capture device.
    static bool startCapture(const std::string& preferred_name);
    // Once per frame while capturing: drains the device into memory.
    static void pollCapture();
    static bool capturing();
    static F32 captureSeconds();
    // Stops and closes the device (the OS mic indicator goes off) and returns the audio.
    static std::vector<S16> stopCapture();
    // Push-to-talk clips have silence at both ends: trims it (energy-based, 20 ms frames,
    // threshold above the clip's own noise floor, 200 ms kept around speech). False = no
    // speech in the clip (too short / too quiet): nothing to transcribe.
    static bool trimSilence(std::vector<S16>& pcm);

    // ---- Transcription (worker thread) ----
    struct Result
    {
        U32         serial = 0;
        bool        ok = false;
        std::string text;            // cleaned: trimmed, no "[BLANK_AUDIO]" style markers
        F32         audioSec = 0.f;
        F64         inferMs = 0.0;   // whisper_full (plus the model load on first use)
        std::string error;           // !ok: why (logged)
    };
    // Queues one clip (16 kHz mono) for `model`. `vocabulary`: words the speaker is likely to say
    // (names, places, the user's own terms), most important first; the worker turns as many as fit
    // a short token budget into Whisper's initial prompt, which biases the decoder toward those
    // spellings. The prompt is never logged (only its token count). Returns the clip's serial
    // (> 0); results come back in order through pollResult(). 0 = not available in this build.
    static U32 transcribe(std::vector<S16>&& pcm, const std::string& language, const Model& model,
                          std::vector<std::string>&& vocabulary);
    static bool pollResult(Result& out);
    static bool busy();   // a clip is queued or being transcribed
    // Built with whisper.cpp (LL_WHISPER) and the CPU has AVX2 / FMA / F16C / BMI2. (A module
    // missing from the installation shows up as a failed Result.)
    static bool available();

    // Frees the model and stops the worker (an unfinished transcription is aborted, its
    // result dropped) and closes a capture device. The VR session ended.
    static void release();
    // LLAppViewer::cleanup: release(), and an unfinished download stops.
    static void shutdown();
};
