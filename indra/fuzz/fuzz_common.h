/**
 * @file fuzz_common.h
 * @brief Shared setup for the libFuzzer harnesses (mirrors indra/test/test.cpp main()).
 *
 * Header only; every harness calls fuzz::initCommon() from LLVMFuzzerInitialize.
 */
#ifndef LL_FUZZ_COMMON_H
#define LL_FUZZ_COMMON_H

#include "linden_common.h"

#include "llapr.h"
#include "llerrorcontrol.h"
#include "lltrace.h"
#include "lltracethreadrecorder.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace fuzz
{

// An LL_ERRS reachable from input is a remote crash of the viewer: make it a fuzzer crash too.
// (Do not set the log level to NONE: that suppresses the fatal function as well.)
inline void onFatal(const std::string& message)
{
    std::fprintf(stderr, "LL_ERRS reached: %s\n", message.c_str());
    std::fflush(stderr);
    std::abort();
}

// Idempotent. FUZZ_LOG=1 in the environment sends LL_WARNS and above to stderr (debugging a
// crash input); otherwise only LL_ERRS (via onFatal) is live and nothing is formatted.
inline void initCommon()
{
    static bool done = false;
    if (done)
    {
        return;
    }
    done = true;

    ll_init_apr();

    const char* log = std::getenv("FUZZ_LOG");
    const bool logging = log && *log && *log != '0';
    LLError::initForApplication(".", ".", logging);
    LLError::setDefaultLevel(logging ? LLError::LEVEL_WARN : LLError::LEVEL_ERROR);
    LLError::setFatalFunction(onFatal);

    static LLTrace::ThreadRecorder* recorder = new LLTrace::ThreadRecorder();
    LLTrace::set_master_thread_recorder(recorder);
}

} // namespace fuzz

#endif // LL_FUZZ_COMMON_H
