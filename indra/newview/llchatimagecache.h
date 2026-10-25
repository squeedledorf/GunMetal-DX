/**
 * @file llchatimagecache.h
 * @brief Chat inline image previews (#265): URL policy, fetch, decode and the
 *        session thumbnail cache.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_LLCHATIMAGECACHE_H
#define LL_LLCHATIMAGECACHE_H

#include "llsingleton.h"
#include "llpointer.h"
#include "llimage.h"

#include <boost/signals2.hpp>

#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

class LLViewerTexture;

// An animated preview (GIF / WebP): one texture per composited frame and how to play
// them. Frame selection is a pure function of play time, so a preview that is not
// drawn costs nothing.
struct LLChatImageAnim
{
    std::vector<LLPointer<LLViewerTexture> > frames;
    std::vector<F32> delays;   // seconds, one per frame
    F64 duration = 0.0;        // one loop, seconds
    U32 loops = 0;             // times to play; 0 = forever

    bool isAnimated() const { return frames.size() > 1 && delays.size() == frames.size() && duration > 0.0; }
    // Recomputes `duration` from `delays`.
    void updateDuration();
    // Index of the frame shown after `play_time` seconds of playing. `finished` is
    // set once every loop has played: the last frame then stays up.
    size_t frameAt(F64 play_time, bool& finished) const;
    void clear();   // out of line: LLViewerTexture is incomplete here
};

// Web image links in chat, decoded to thumbnails.
//
// Fetching an untrusted URL reveals the viewer's IP address to the site, so the
// decision whether a link loads by itself (friend / trusted site / own message /
// "everyone" setting) is made by the chat widget (llchatimagepreview.cpp); this
// class only fetches what it is asked to, and only after the URL passed
// checkUrl(): http/https, no credentials, no IP-literal host in a private/local
// range, no "localhost". Every redirect hop is re-checked (redirects are followed
// here, not by libcurl, max 3), and the HTTP layer refuses private addresses on
// the resolved socket address too (HttpOptions::setBlockPrivateAddresses).
//
// Bodies are capped at 10 MB, 15 s per hop, Content-Type must be image/png,
// image/jpeg, image/gif or image/webp with matching magic bytes, dimensions <=
// 4096x4096 are checked from the header before decoding. Decoding and downscaling
// run on the "General" work queue; the textures are made on the main thread. GIFs
// decode with Wuffs (llchatimagegif.h), WebP with libwebp (llchatimagewebp.h, #651),
// both under the same limits: an animated GIF / WebP becomes one texture per
// composited frame, at 1x the preview height (not 2x like stills) so a typical
// animation fits; its frames together are capped at half the budget (max 32 MB),
// and frames beyond that (or beyond 300 frames / the decode work limits) are cut,
// the rest loops.
//
// Memory: thumbnails (POT-stretched, <= 2x the preview height; every frame of an
// animation) count against ChatImageCacheMB; the least recently drawn are dropped and their widgets offer
// "Click to reload". Entry bookkeeping is capped by count. The original bytes are
// NOT kept: the preview floater re-fetches the image (user-initiated) and holds
// the bytes only while it is open (for Save).
//
// TODO(#265 follow-up): SL texture previews (secondlife:///app/... texture links /
// UUIDs) would plug in at LLChatImagePreview::appendPreviews and fetch via
// LLViewerTextureManager instead of HTTP.
class LLChatImageCache : public LLSingleton<LLChatImageCache>
{
    LLSINGLETON(LLChatImageCache);
    ~LLChatImageCache();

public:
    enum EState
    {
        STATE_NONE = 0,     // never requested, or dropped from the cache
        STATE_QUEUED,
        STATE_FETCHING,
        STATE_DECODING,
        STATE_READY,
        STATE_FAILED
    };

    // ---- URL policy (pure, main thread) ----------------------------------------
    // Parses an http(s) URL; false (with a short user-facing reason) when it must
    // never be fetched. `host` is lower-cased, without port.
    static bool checkUrl(const std::string& url, std::string& host, std::string& reason);
    // Worth previewing at all: path ends in .png/.jpg/.jpeg/.gif/.webp, or a known
    // image host whose links have no extension.
    static bool looksLikeImage(const std::string& url, const std::string& host);
    // Host is on the ChatImageTrustedHosts list (exact or subdomain).
    static bool isTrustedHost(const std::string& host);

    // ---- thumbnails -------------------------------------------------------------
    EState getState(const std::string& url, std::string* reason = nullptr) const;
    // READY thumbnail (touches the LRU). out_w/out_h = original image size, the
    // texture is stretched to power-of-two dims so draw it into an aspect-correct
    // rect computed from these. For an animated GIF / WebP this is the first frame and
    // `anim` (when given) receives the animation (else null); the pointer is valid
    // until the cache changes (use it right away, don't keep it).
    LLViewerTexture* getThumbnail(const std::string& url, S32& out_w, S32& out_h,
                                  const LLChatImageAnim** anim = nullptr);
    // Queue a thumbnail fetch (no-op when queued, loading, ready or failed).
    // False when the queue is full (nothing recorded; ask again later).
    bool request(const std::string& url);
    // Forget a failed entry so request() tries again.
    void clearFailure(const std::string& url);

    // Fired (main thread) whenever an entry changes state or is dropped.
    typedef boost::signals2::signal<void(const std::string& url)> update_signal_t;
    boost::signals2::connection addUpdateCallback(const update_signal_t::slot_type& cb)
    {
        return mUpdateSignal.connect(cb);
    }

    // ---- full image for the preview floater (not cached) -------------------------
    struct FullResult
    {
        bool ok = false;
        std::string reason;
        LLPointer<LLImageRaw> raw;                  // POT-stretched, <= 2048 per side (first frame)
        std::vector<LLPointer<LLImageRaw> > frames; // animated GIF / WebP: all frames (<= 1024 per side), raw = frames[0]
        std::vector<F32> delays;                    // seconds, one per frame
        U32 loops = 0;                              // times to play; 0 = forever
        bool truncated = false;                     // animation cut short by the limits
        S32 orig_w = 0, orig_h = 0;
        std::string mime;                           // image/png, image/jpeg, image/gif or image/webp
        std::shared_ptr<std::vector<U8> > bytes;    // original fetched bytes (Save)
    };
    typedef std::function<void(const FullResult&)> full_callback_t;
    // Returns a ticket for cancelFull(); the callback runs on the main thread.
    U32 requestFull(const std::string& url, const full_callback_t& cb);
    void cancelFull(U32 ticket);

    // Bytes held by open preview floaters (reported in the stats line).
    void addFloaterBytes(S64 delta);

    // live/cap/peak line to the log.
    void logStats(const std::string& why) const;

    // Detection constants (also used by the widget).
    static constexpr S32 MAX_REDIRECTS = 3;
    static constexpr S32 MAX_IN_FLIGHT = 4;
    static constexpr S32 MAX_QUEUED = 64;
    static constexpr S32 MAX_ENTRIES = 1024;
    static constexpr size_t MAX_BODY_BYTES = size_t(10) * 1024 * 1024;
    static constexpr S32 MAX_DIMENSION = 4096;
    static constexpr U32 TIMEOUT_SECS = 15;
    static constexpr S32 THUMB_MAX_WIDTH = 720;     // texels (2x a 360 px column)
    static constexpr S32 FULL_MAX_SIDE = 2048;      // texels for the preview floater
    static constexpr S32 FULL_ANIM_MAX_SIDE = 1024; // texels per animated frame in the floater
    static constexpr U64 FULL_ANIM_MAX_BYTES = U64(96) * 1024 * 1024;   // all frames, one floater
    static constexpr U64 THUMB_ANIM_MAX_BYTES = U64(32) * 1024 * 1024;  // all frames, one thumbnail

private:
    struct Entry
    {
        EState state = STATE_NONE;
        std::string reason;
        LLPointer<LLViewerTexture> thumb;   // the still, or an animation's first frame
        LLChatImageAnim anim;               // animated GIF / WebP (else empty)
        S32 orig_w = 0, orig_h = 0;
        U64 bytes = 0;           // CPU bytes of the thumbnail texture copies (all frames)
        U64 last_used = 0;       // LRU tick
    };

    struct Job
    {
        std::string url;
        U32 ticket = 0;          // 0 = thumbnail, else a full request
    };

    // Result of one fetch (redirects resolved).
    struct FetchOutcome
    {
        bool ok = false;
        std::string reason;
        std::string mime;
        std::shared_ptr<std::vector<U8> > bytes;
    };

    // Result of decode + downscale (worker thread -> main thread).
    struct DecodeOutcome
    {
        bool ok = false;
        std::string reason;
        LLPointer<LLImageRaw> raw;                  // the still, or frames[0]
        std::vector<LLPointer<LLImageRaw> > frames; // GIF / WebP: every frame (one for a still)
        std::vector<F32> delays;
        U32 loops = 0;
        bool truncated = false;
        S32 orig_w = 0, orig_h = 0;
    };

    // Output sizes for one decode (computed on the main thread: reads settings).
    struct DecodeParams
    {
        S32 box_w = 0, box_h = 0, pot_max = 0;   // stills (and the first frame)
        S32 anim_box_w = 0, anim_box_h = 0;      // animated GIF / WebP frames
        U64 anim_max_bytes = 0;                  // all animated frames together
    };

    void pump();
    void startJob(const Job& job);
    static void fetchCoro(Job job);
    static FetchOutcome fetchBytes(const std::string& url);
    static DecodeOutcome decode(const std::shared_ptr<std::vector<U8> >& bytes, const std::string& mime,
                                const DecodeParams& params);
    void onFetchFailed(const Job& job, const std::string& reason);
    void onDecoded(const Job& job, const DecodeOutcome& out, const FetchOutcome& fetched);
    void finishJob();

    void setState(const std::string& url, EState state, const std::string& reason = std::string());
    void notify(const std::string& url);
    void enforceBudget(const std::string& keep_url);
    void enforceEntryCap();
    U64 budgetBytes() const;
    U64 nextTick() { return ++mTick; }

    std::unordered_map<std::string, Entry> mEntries;
    std::deque<Job> mQueue;
    S32 mInFlight = 0;
    U64 mTick = 0;

    std::map<U32, full_callback_t> mFullCallbacks;
    U32 mNextTicket = 1;

    U64 mThumbBytes = 0;
    U64 mPeakThumbBytes = 0;
    S64 mFloaterBytes = 0;
    U32 mStatFetches = 0, mStatFailures = 0, mStatEvictions = 0;

    update_signal_t mUpdateSignal;
    boost::signals2::scoped_connection mBudgetConnection;
};

#endif // LL_LLCHATIMAGECACHE_H
