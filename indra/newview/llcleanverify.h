/**
 * @file llcleanverify.h
 * @brief LL-render-removal runtime verification (artifact #3 of 4; see
 *        the clean-pipeline-only rule). Every LL scene-draw body that is supposed to
 *        be unreachable calls CLEAN_VERIFY_LL_DRAW_UNREACHABLE(site) at its
 *        top. Zero cost while the invariant holds (the call never executes);
 *        if a gate ever breaks, a #CleanViolation# WARNS fires once per site
 *        and a counter increments -- proof, not guesswork, that an LL draw
 *        ran. This is an assertion, not a diagnostic: it emits nothing in the
 *        expected (clean-only) state.
 */

#pragma once

#include "llerror.h"
#include <atomic>

namespace CleanVerify
{
inline std::atomic<unsigned int> sLLDrawViolations{0};

inline void llDrawUnreachable(const char* site)
{
    sLLDrawViolations.fetch_add(1u, std::memory_order_relaxed);
    static thread_local const char* s_last = nullptr;
    if (s_last != site)   // once per site per thread, not per frame
    {
        s_last = site;
        LL_WARNS("CleanVerify") << "#CleanViolation# LL scene draw executed at "
            << site << " (total=" << sLLDrawViolations.load() << ")" << LL_ENDL;
    }
}
} // namespace CleanVerify

#define CLEAN_VERIFY_LL_DRAW_UNREACHABLE(site) CleanVerify::llDrawUnreachable(site)
