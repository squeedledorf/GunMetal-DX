/**
 * @file llapp.cpp
 * @brief Implementation of the LLApp class.
 *
 * $LicenseInfo:firstyear=2003&license=viewerlgpl$
 * Second Life Viewer Source Code
 * Copyright (C) 2010, Linden Research, Inc.
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
 *
 * Linden Research, Inc., 945 Battery Street, San Francisco, CA  94111  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "llapp.h"
#if LL_LINUX
#include <execinfo.h>
#include <ucontext.h>
#include <unistd.h>
#include <link.h>
#include <elf.h>
#include <string.h>
#endif

#include <atomic>
#include <cstdlib>
#include <cstring>

#ifdef LL_DARWIN
#include <sys/types.h>
#include <unistd.h>
#include <sys/sysctl.h>
#endif

#include "llcommon.h"
#include "llapr.h"
#include "llerrorcontrol.h"
#include "llframetimer.h"
#include "lllivefile.h"
#include "llmemory.h"
#include "llstl.h" // for DeletePointer()
#include "llstring.h"
#include "lleventtimer.h"
#include "stringize.h"
#include "llcleanup.h"
#include "llevents.h"
#include "llsdutil.h"

//
// Signal handling
#ifndef LL_WINDOWS
# include <signal.h>
# include <unistd.h> // for fork()
void setup_signals();
void default_unix_signal_handler(int signum, siginfo_t *info, void *);

#if LL_LINUX
#else
// Called by breakpad exception handler after the minidump has been generated.
bool unix_post_minidump_callback(const char *dump_dir,
                      const char *minidump_id,
                      void *context, bool succeeded);
#endif

# if LL_DARWIN
/* OSX doesn't support SIGRT* */
S32 LL_SMACKDOWN_SIGNAL = SIGUSR1;
S32 LL_HEARTBEAT_SIGNAL = SIGUSR2;
# else // linux or (assumed) other similar unixoid
/* We want reliable delivery of our signals - SIGRT* is it. */
/* Old LinuxThreads versions eat SIGRTMIN+0 to SIGRTMIN+2, avoid those. */
/* Note that SIGRTMIN/SIGRTMAX may expand to a glibc function call with a
   nonconstant result so these are not consts and cannot be used in constant-
   expressions.  SIGRTMAX may return -1 on rare broken setups. */
S32 LL_SMACKDOWN_SIGNAL = (SIGRTMAX >= 0) ? (SIGRTMAX-1) : SIGUSR1;
S32 LL_HEARTBEAT_SIGNAL = (SIGRTMAX >= 0) ? (SIGRTMAX-0) : SIGUSR2;
# endif // LL_DARWIN
#endif // !LL_WINDOWS

// the static application instance
LLApp* LLApp::sApplication = NULL;

// Allows the generation of core files for post mortem under gdb
// and disables crashlogger
bool LLApp::sDisableCrashlogger = false;

// static
// Keeps track of application status
LLScalarCond<LLApp::EAppStatus> LLApp::sStatus{LLApp::APP_STATUS_STOPPED};
LLAppErrorHandler LLApp::sErrorHandler = NULL;
static void (*sFatalSignalHook)() = NULL;   // #876 (set before threads fault; read in the fatal path)
void LLApp::setFatalSignalHook(void (*hook)()) { sFatalSignalHook = hook; }


LLApp::LLApp()
{
    // Set our status to running
    setStatus(APP_STATUS_RUNNING);

    LLCommon::initClass();

    // initialize the options structure. We need to make this an array
    // because the structured data will not auto-allocate if we
    // reference an invalid location with the [] operator.
    mOptions = LLSD::emptyArray();
    LLSD sd;
    for(int i = 0; i < PRIORITY_COUNT; ++i)
    {
        mOptions.append(sd);
    }

    // Make sure we clean up APR when we exit
    // Don't need to do this if we're cleaning up APR in the destructor
    //atexit(ll_cleanup_apr);

    // Set the application to this instance.
    sApplication = this;

    // initialize the buffer to write the minidump filename to
    // (this is used to avoid allocating memory in the crash handler)
    memset(mMinidumpPath, 0, MAX_MINDUMP_PATH_LENGTH);
    mCrashReportPipeStr = L"\\\\.\\pipe\\LLCrashReporterPipe";
}


LLApp::~LLApp()
{

    // reclaim live file memory
    std::for_each(mLiveFiles.begin(), mLiveFiles.end(), DeletePointer());
    mLiveFiles.clear();

    setStopped();

    SUBSYSTEM_CLEANUP_DBG(LLCommon);
}

// static
LLApp* LLApp::instance()
{
    return sApplication;
}


LLSD LLApp::getOption(const std::string& name) const
{
    LLSD rv;
    LLSD::array_const_iterator iter = mOptions.beginArray();
    LLSD::array_const_iterator end = mOptions.endArray();
    for(; iter != end; ++iter)
    {
        rv = (*iter)[name];
        if(rv.isDefined()) break;
    }
    return rv;
}

bool LLApp::parseCommandOptions(int argc, char** argv)
{
    LLSD commands;
    std::string name;
    std::string value;
    for(int ii = 1; ii < argc; ++ii)
    {
        if(argv[ii][0] != '-')
        {
            LL_INFOS() << "Did not find option identifier while parsing token: "
                << argv[ii] << LL_ENDL;
            return false;
        }
        int offset = 1;
        if(argv[ii][1] == '-') ++offset;
        name.assign(&argv[ii][offset]);
        if(((ii+1) >= argc) || (argv[ii+1][0] == '-'))
        {
            // we found another option after this one or we have
            // reached the end. simply record that this option was
            // found and continue.
            int flag = name.compare("logfile");
            if (0 == flag)
            {
                commands[name] = "log";
            }
            else
            {
                commands[name] = true;
            }

            continue;
        }
        ++ii;
        value.assign(argv[ii]);

#if LL_WINDOWS
        //Windows changed command line parsing.  Deal with it.
        size_t slen = value.length() - 1;
        size_t start = 0;
        size_t end = slen;
        if (argv[ii][start]=='"')start++;
        if (argv[ii][end]=='"')end--;
        if (start!=0 || end!=slen)
        {
            value = value.substr (start,end);
        }
#endif

        commands[name] = value;
    }
    setOptionData(PRIORITY_COMMAND_LINE, commands);
    return true;
}

bool LLApp::parseCommandOptions(int argc, wchar_t** wargv)
{
    LLSD commands;
    std::string name;
    std::string value;
    for(int ii = 1; ii < argc; ++ii)
    {
        if(wargv[ii][0] != '-')
        {
            LL_INFOS() << "Did not find option identifier while parsing token: "
                << (intptr_t)wargv[ii] << LL_ENDL;
            return false;
        }
        int offset = 1;
        if(wargv[ii][1] == '-') ++offset;

#if LL_WINDOWS
    name.assign(ll_convert_wide_to_string(&wargv[ii][offset]));
#else
    name.assign(wstring_to_utf8str(&wargv[ii][offset]));
#endif
        if(((ii+1) >= argc) || (wargv[ii+1][0] == '-'))
        {
            // we found another option after this one or we have
            // reached the end. simply record that this option was
            // found and continue.
            int flag = name.compare("logfile");
            if (0 == flag)
            {
                commands[name] = "log";
            }
            else
            {
                commands[name] = true;
            }

            continue;
        }
        ++ii;

#if LL_WINDOWS
    value.assign(ll_convert_wide_to_string((wargv[ii])));
#else
    value.assign(wstring_to_utf8str((wargv[ii])));
#endif

#if LL_WINDOWS
        //Windows changed command line parsing.  Deal with it.
        size_t slen = value.length() - 1;
        size_t start = 0;
        size_t end = slen;
        if (wargv[ii][start]=='"')start++;
        if (wargv[ii][end]=='"')end--;
        if (start!=0 || end!=slen)
        {
            value = value.substr (start,end);
        }
#endif

        commands[name] = value;
    }
    setOptionData(PRIORITY_COMMAND_LINE, commands);
    return true;
}

void LLApp::manageLiveFile(LLLiveFile* livefile)
{
    if(!livefile) return;
    livefile->checkAndReload();
    livefile->addToEventTimer();
    mLiveFiles.push_back(livefile);
}

bool LLApp::setOptionData(OptionPriority level, LLSD data)
{
    if((level < 0)
       || (level >= PRIORITY_COUNT)
       || (data.type() != LLSD::TypeMap))
    {
        return false;
    }
    mOptions[level] = data;
    return true;
}

LLSD LLApp::getOptionData(OptionPriority level)
{
    if((level < 0) || (level >= PRIORITY_COUNT))
    {
        return LLSD();
    }
    return mOptions[level];
}

void LLApp::stepFrame()
{
    LLFrameTimer::updateFrameTime();
    LLFrameTimer::updateFrameCount();
    LLEventTimer::updateClass();
    mRunner.run();
}

void LLApp::setupErrorHandling(bool second_instance)
{
    // Error handling is done by starting up an error handling thread, which just sleeps and
    // occasionally checks to see if the app is in an error state, and sees if it needs to be run.

#if LL_WINDOWS

#else  // ! LL_WINDOWS

    //
    // Start up signal handling.
    //
    // There are two different classes of signals.  Synchronous signals are delivered to a specific
    // thread, asynchronous signals can be delivered to any thread (in theory)
    //
    setup_signals();

#endif // ! LL_WINDOWS
}

void LLApp::setErrorHandler(LLAppErrorHandler handler)
{
    LLApp::sErrorHandler = handler;
}

// static
void LLApp::runErrorHandler()
{
    if (LLApp::sErrorHandler)
    {
        LLApp::sErrorHandler();
    }

    //LL_INFOS() << "App status now STOPPED" << LL_ENDL;
    LLApp::setStopped();
}

namespace
{

static std::map<LLApp::EAppStatus, const char*> statusDesc
{
    { LLApp::APP_STATUS_RUNNING,  "running" },
    { LLApp::APP_STATUS_QUITTING, "quitting" },
    { LLApp::APP_STATUS_STOPPED,  "stopped" },
    { LLApp::APP_STATUS_ERROR,    "error" }
};

} // anonymous namespace

// static
void LLApp::setStatus(EAppStatus status)
{
    auto status_it = statusDesc.find(status);
    std::string status_text = status_it != statusDesc.end() ? std::string(status_it->second) : std::to_string(status);
    LL_INFOS() << "status: " << status_text << LL_ENDL;
    // notify everyone waiting on sStatus any time its value changes
    sStatus.set_all(status);

    // This can also happen very late in the application lifecycle -- don't
    // resurrect a deleted LLSingleton
    if (! LLEventPumps::wasDeleted())
    {
        // notify interested parties of status change
        LLEventPumps::instance().obtain("LLApp").post(llsd::map("status", status_text));
    }
}


// static
void LLApp::setError()
{
    // set app status to ERROR
    setStatus(APP_STATUS_ERROR);
}

void LLApp::setDebugFileNames(const std::string &path)
{
    mStaticDebugFileName = path + "static_debug_info.log";
    mDynamicDebugFileName = path + "dynamic_debug_info.log";
}

void LLApp::writeMiniDump()
{
}

// static
void LLApp::setQuitting()
{
    if (!isExiting())
    {
        // If we're already exiting, we don't want to reset our state back to quitting.
        LL_INFOS() << "Setting app state to QUITTING" << LL_ENDL;
        setStatus(APP_STATUS_QUITTING);
    }
}


// static
void LLApp::setStopped()
{
    setStatus(APP_STATUS_STOPPED);
}


// static
bool LLApp::isStopped()
{
    return (APP_STATUS_STOPPED == sStatus.get());
}


// static
bool LLApp::isRunning()
{
    return (APP_STATUS_RUNNING == sStatus.get());
}


// static
bool LLApp::isError()
{
    return (APP_STATUS_ERROR == sStatus.get());
}


// static
bool LLApp::isQuitting()
{
    return (APP_STATUS_QUITTING == sStatus.get());
}

// static
bool LLApp::isExiting()
{
    return isQuitting() || isError();
}

void LLApp::disableCrashlogger()
{
    sDisableCrashlogger = true;
}

// static
bool LLApp::isCrashloggerDisabled()
{
    return sDisableCrashlogger;
}

// static
int LLApp::getPid()
{
#if LL_WINDOWS
    return GetCurrentProcessId();
#else
    return getpid();
#endif
}

// static
void LLApp::notifyOutOfDiskSpace()
{
    static const U32Seconds min_interval = U32Seconds(60);
    static U32Seconds min_time_to_send = U32Seconds(0);
    U32Seconds now = LLTimer::getTotalTime();
    if (now < min_time_to_send)
        return;

    min_time_to_send = now + min_interval;

    if (LLApp* app = instance())
    {
        app->sendOutOfDiskSpaceNotification();
    }
    else
    {
        LL_WARNS() << "No app instance" << LL_ENDL;
    }
}

// virtual
void LLApp::sendOutOfDiskSpaceNotification()
{
    LL_WARNS() << "Should never be called" << LL_ENDL; // Should be overridden
}

#ifndef LL_WINDOWS
// ---------------------------------------------------------------------------
// Fatal-signal handling invariants (2026-10-08 hang: a SIGSEGV raised from the
// std::terminate path spun the main thread at 100% CPU inside this handler for
// 15+ minutes and never produced a core):
//
//  * Everything on the fatal path must be async-signal-safe. LLApp::setError()
//    is NOT: it logs (LLError mutexes/streams), locks sStatus (a
//    boost::fibers::mutex whose internal spinlock_ttas busy-waits with
//    sched_yield -- 100% CPU forever if the interrupted frame holds it, and
//    std::terminate does not unwind, so every lock live at the throw stays held),
//    then broadcasts on the "LLApp" LLEventPump, whose listeners close work
//    queues, set fiber promises and JOIN ThreadPool threads. None of that is
//    needed to die, so the fatal path no longer calls it.
//  * "Already handling a fatal signal" is a lock-free atomic flag, not
//    LLApp::isError() (which takes the same fibers mutex).
//  * A watchdog (alarm(2) + a tiny SIGALRM handler) is armed before anything
//    non-trivial, so if the handler blocks or spins regardless, the process
//    dies with a core within sFatalWatchdogSecs. SIGALRM's default action has no
//    core, so the watchdog re-raises the original fatal signal at SIG_DFL.
//  * Any further fatal signal (or SIGINT/SIGTERM) while handling one goes
//    straight to SIG_DFL + unblock + raise.
// ---------------------------------------------------------------------------
namespace
{
    // Signal number being handled, 0 = none. A lock-free atomic (async-signal-safe)
    // so two threads faulting at once cannot both take the fatal path.
    std::atomic<int> sFatalSignalInProgress{ 0 };
    static_assert(std::atomic<int>::is_always_lock_free, "fatal-signal flag must be lock-free");
    volatile sig_atomic_t sWatchdogSignal = 0;        // fatal signal the watchdog re-raises
    unsigned sFatalWatchdogSecs = 5;                  // SLIPSTREAM_FATAL_WATCHDOG_SECS
    bool sFatalHandlerTestHang = false;               // SLIPSTREAM_FATAL_HANDLER_TEST=hang

    // Main-executable identity for the fatal-context header, computed once at
    // setup_signals() time (dl_iterate_phdr is not async-signal-safe).
#if LL_LINUX
    char sExeBuildId[2 * 40 + 1] = { 0 };   // GNU build-id, hex
    char sExePath[512] = { 0 };
    unsigned long long sExeBase = 0;       // PIE load bias: in-binary address = addr - base
    unsigned long long sExeEnd = 0;        // end of the highest PT_LOAD (runtime address)
#endif

    void fatal_write(const char* str)
    {
        (void)!write(STDERR_FILENO, str, strlen(str));
    }

    // "0x" + 16 hex digits, NUL-terminated into out[19]
    void fatal_hex(unsigned long long v, char* out)
    {
        static const char d[] = "0123456789abcdef";
        out[0] = '0'; out[1] = 'x';
        for (int i = 0; i < 16; ++i) out[2 + i] = d[(v >> ((15 - i) * 4)) & 15];
        out[18] = 0;
    }

    bool is_fatal_signal(int signum)
    {
        return signum == SIGSEGV || signum == SIGBUS || signum == SIGILL || signum == SIGFPE ||
               signum == SIGABRT || signum == SIGQUIT || signum == SIGSYS ||
               signum == LL_SMACKDOWN_SIGNAL;
    }

    // Terminate NOW with the default action (core) for signum. Async-signal-safe.
    // The signal being handled is blocked while its handler runs (no SA_NODEFER),
    // so a bare raise() would only leave it pending: reset to SIG_DFL, unblock, raise.
    [[noreturn]] void fatal_die_now(int signum)
    {
        if (signum == LL_SMACKDOWN_SIGNAL || signum <= 0)
        {
            signum = SIGABRT; // smackdown's default action has no core
        }
        struct sigaction act;
        memset(&act, 0, sizeof(act));
        act.sa_handler = SIG_DFL;
        sigemptyset(&act.sa_mask);
        sigaction(signum, &act, NULL);
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, signum);
        pthread_sigmask(SIG_UNBLOCK, &set, NULL);
        raise(signum);
        // Only reachable if the default action somehow did not terminate us.
        sigaction(SIGABRT, &act, NULL);
        sigemptyset(&set);
        sigaddset(&set, SIGABRT);
        pthread_sigmask(SIG_UNBLOCK, &set, NULL);
        raise(SIGABRT);
        _exit(128 + signum);
    }

    void fatal_watchdog_handler(int)
    {
        fatal_write("\n*** fatal signal handler did not finish within the watchdog timeout; "
                    "dumping core now ***\n");
        fatal_die_now(sWatchdogSignal);
    }

    // Arm the watchdog: within sFatalWatchdogSecs, whatever is blocking or
    // spinning, the process dies with a core for `signum`. alarm() is
    // process-directed: the kernel delivers it to the main thread or any other
    // thread not blocking SIGALRM, and a core-dumping signal raised on any
    // thread takes down (and dumps) the whole process.
    void arm_fatal_watchdog(int signum)
    {
        sWatchdogSignal = signum;
        struct sigaction act;
        memset(&act, 0, sizeof(act));
        act.sa_handler = fatal_watchdog_handler;
        sigemptyset(&act.sa_mask);
        act.sa_flags = 0;
        sigaction(SIGALRM, &act, NULL);
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, SIGALRM);
        pthread_sigmask(SIG_UNBLOCK, &set, NULL);
        alarm(sFatalWatchdogSecs);
    }

    // First entry for a fatal signal: claim the in-progress flag, make a fault
    // inside the handler go straight to core, arm the watchdog. Returns false if
    // a fatal signal was already in progress (caller must fatal_die_now()).
    bool begin_fatal_signal(int signum)
    {
        int expected = 0;
        if (!sFatalSignalInProgress.compare_exchange_strong(expected, signum))
        {
            return false;
        }
        struct sigaction act;
        memset(&act, 0, sizeof(act));
        act.sa_handler = SIG_DFL;
        sigemptyset(&act.sa_mask);
        sigaction(signum == LL_SMACKDOWN_SIGNAL ? SIGABRT : signum, &act, NULL);
        arm_fatal_watchdog(signum);
        if (sFatalHandlerTestHang)
        {
            fatal_write("\n*** SLIPSTREAM_FATAL_HANDLER_TEST=hang: spinning in the fatal handler "
                        "until the watchdog fires ***\n");
            for (volatile int spin = 0; ; spin = spin + 1) {}
        }
        return true;
    }

    // Header naming the exact binary, so the in-binary offsets that follow can be
    // symbolized after later builds overwrite the build tree. The debug info is
    // archived per package by viewer_manifest.py (LinuxManifest.archive_debug_symbols) at
    //   build-linux-x86_64/symbols/.build-id/<id[0:2]>/<id[2:]>.debug
    // Symbolize an in-binary offset (the "exe+0x..." values below, or the (+0x...)
    // of backtrace_symbols_fd frames for the main executable):
    //   addr2line -Cfip -e build-linux-x86_64/symbols/.build-id/ab/cdef....debug 0xOFFSET
    // With a core (gdb/coredumpctl match the debug file by build-id):
    //   gdb -iex 'set debug-file-directory build-linux-x86_64/symbols' <binary> <core>
    void fatal_write_exe_header()
    {
#if LL_LINUX
        char h[19];
        fatal_write("*** exe: ");
        fatal_write(sExePath[0] ? sExePath : "(unknown)");
        fatal_write("\n*** build-id: ");
        fatal_write(sExeBuildId[0] ? sExeBuildId : "(none)");
        fatal_write("  load base: ");
        fatal_hex(sExeBase, h);
        fatal_write(h);
        fatal_write("  (in-binary offset = address - load base)\n");
        if (sExeBuildId[0] && sExeBuildId[1])
        {
            const char two[3] = { sExeBuildId[0], sExeBuildId[1], 0 };
            fatal_write("*** debug file: build-linux-x86_64/symbols/.build-id/");
            fatal_write(two);
            fatal_write("/");
            fatal_write(sExeBuildId + 2);
            fatal_write(".debug  (addr2line -Cfip -e <debug file> <offset>)\n");
        }
#endif
    }

    // " exe+0x..." for an address inside the main executable's mapping, else nothing.
    [[maybe_unused]] void fatal_write_exe_offset(unsigned long long addr)
    {
#if LL_LINUX
        if (sExeEnd && addr >= sExeBase && addr < sExeEnd)
        {
            char h[19];
            fatal_hex(addr - sExeBase, h);
            fatal_write(" exe+");
            fatal_write(h);
        }
#else
        (void)addr;
#endif
    }

#if LL_LINUX
    int find_main_exe_cb(struct dl_phdr_info* info, size_t, void*)
    {
        // The first object dl_iterate_phdr reports is the main program.
        sExeBase = (unsigned long long)info->dlpi_addr;
        for (int i = 0; i < info->dlpi_phnum; ++i)
        {
            const ElfW(Phdr)& ph = info->dlpi_phdr[i];
            if (ph.p_type == PT_LOAD)
            {
                const unsigned long long end = sExeBase + ph.p_vaddr + ph.p_memsz;
                if (end > sExeEnd) sExeEnd = end;
            }
            else if (ph.p_type == PT_NOTE && !sExeBuildId[0])
            {
                const char* p = (const char*)(uintptr_t)(sExeBase + ph.p_vaddr);
                const char* const e = p + ph.p_memsz;
                while (p + sizeof(ElfW(Nhdr)) <= e)
                {
                    const ElfW(Nhdr)* nh = (const ElfW(Nhdr)*)p;
                    const char* name = p + sizeof(ElfW(Nhdr));
                    const char* desc = name + ((nh->n_namesz + 3) & ~3u);
                    const char* next = desc + ((nh->n_descsz + 3) & ~3u);
                    if (next > e) break;
                    if (nh->n_type == NT_GNU_BUILD_ID && nh->n_namesz == 4 && memcmp(name, "GNU", 4) == 0)
                    {
                        static const char d[] = "0123456789abcdef";
                        size_t n = nh->n_descsz;
                        if (n > (sizeof(sExeBuildId) - 1) / 2) n = (sizeof(sExeBuildId) - 1) / 2;
                        for (size_t k = 0; k < n; ++k)
                        {
                            const unsigned char b = (unsigned char)desc[k];
                            sExeBuildId[2 * k] = d[b >> 4];
                            sExeBuildId[2 * k + 1] = d[b & 15];
                        }
                        sExeBuildId[2 * n] = 0;
                        break;
                    }
                    p = next;
                }
            }
        }
        return 1; // stop after the main program
    }
#endif

    void init_fatal_signal_support()
    {
        if (const char* secs = getenv("SLIPSTREAM_FATAL_WATCHDOG_SECS"))
        {
            const int v = atoi(secs);
            if (v > 0) sFatalWatchdogSecs = (unsigned)v;
        }
        if (const char* test = getenv("SLIPSTREAM_FATAL_HANDLER_TEST"))
        {
            sFatalHandlerTestHang = (strcmp(test, "hang") == 0);
        }
#if LL_LINUX
        if (!sExeEnd)
        {
            dl_iterate_phdr(find_main_exe_cb, nullptr);
        }
        if (!sExePath[0])
        {
            const ssize_t n = readlink("/proc/self/exe", sExePath, sizeof(sExePath) - 1);
            sExePath[n > 0 ? n : 0] = 0;
        }
#endif
    }
} // anonymous namespace

void setup_signals()
{
    init_fatal_signal_support();
    //
    // Set up signal handlers that may result in program termination
    //
#if LL_LINUX
    {
        // Prime backtrace() now: its first call dlopens libgcc (not signal-safe).
        void* prime[2];
        backtrace(prime, 2);
    }
#endif
    struct sigaction act;
    act.sa_sigaction = default_unix_signal_handler;
    sigemptyset( &act.sa_mask );
    act.sa_flags = SA_SIGINFO;

    // Synchronous signals
    sigaction(SIGABRT, &act, NULL);
    sigaction(SIGALRM, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
    sigaction(SIGFPE, &act, NULL);
    sigaction(SIGHUP, &act, NULL);
    sigaction(SIGILL, &act, NULL);
    sigaction(SIGPIPE, &act, NULL);
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGSYS, &act, NULL);

    sigaction(LL_HEARTBEAT_SIGNAL, &act, NULL);
    sigaction(LL_SMACKDOWN_SIGNAL, &act, NULL);

    // Asynchronous signals that are normally ignored
#ifndef LL_IGNORE_SIGCHLD
    sigaction(SIGCHLD, &act, NULL);
#endif // LL_IGNORE_SIGCHLD
    sigaction(SIGUSR2, &act, NULL);

    // Asynchronous signals that result in attempted graceful exit
    sigaction(SIGHUP, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT, &act, NULL);

    // Asynchronous signals that result in core
    sigaction(SIGQUIT, &act, NULL);

}

void clear_signals()
{
    struct sigaction act;
    act.sa_handler = SIG_DFL;
    sigemptyset( &act.sa_mask );
    act.sa_flags = SA_SIGINFO;

    // Synchronous signals
    sigaction(SIGABRT, &act, NULL);
    sigaction(SIGALRM, &act, NULL);
    sigaction(SIGBUS, &act, NULL);
    sigaction(SIGFPE, &act, NULL);
    sigaction(SIGHUP, &act, NULL);
    sigaction(SIGILL, &act, NULL);
    sigaction(SIGPIPE, &act, NULL);
    sigaction(SIGSEGV, &act, NULL);
    sigaction(SIGSYS, &act, NULL);

    sigaction(LL_HEARTBEAT_SIGNAL, &act, NULL);
    sigaction(LL_SMACKDOWN_SIGNAL, &act, NULL);

    // Asynchronous signals that are normally ignored
#ifndef LL_IGNORE_SIGCHLD
    sigaction(SIGCHLD, &act, NULL);
#endif // LL_IGNORE_SIGCHLD

    // Asynchronous signals that result in attempted graceful exit
    sigaction(SIGHUP, &act, NULL);
    sigaction(SIGTERM, &act, NULL);
    sigaction(SIGINT, &act, NULL);

    // Asynchronous signals that result in core
    sigaction(SIGUSR2, &act, NULL);
    sigaction(SIGQUIT, &act, NULL);
}



void default_unix_signal_handler(int signum, siginfo_t *info, void *uctx)
{
    // Unix implementation of the signal handler. Fatal signals run in the thread
    // that faulted (or, for kill(1)-sent ones, whichever thread the kernel picked).
    // See the "Fatal-signal handling invariants" block above setup_signals(): the
    // fatal path is async-signal-safe only, watchdog-guarded, and ends in
    // fatal_die_now() (SIG_DFL + unblock + raise => core).

    if (const int in_progress = sFatalSignalInProgress.load())
    {
        // Already dying. Any further fatal signal -- or an INT/TERM from someone
        // trying to get rid of us -- dumps core immediately; nothing else may
        // disturb the dump.
        if (is_fatal_signal(signum))
        {
            fatal_die_now(signum);
        }
        if (signum == SIGINT || signum == SIGTERM)
        {
            fatal_die_now(in_progress);
        }
        return;
    }

    if (is_fatal_signal(signum))
    {
        if (!begin_fatal_signal(signum))
        {
            // Another thread claimed the fatal path between the check above and now.
            fatal_die_now(signum);
        }
        // From here on: write(2)-only output, no LLError, no LLApp status, no locks.
        // The watchdog is armed, so if backtrace() (which takes the dynamic
        // loader's lock) or anything else wedges, we still core within seconds.
        if (sFatalSignalHook && signum != LL_SMACKDOWN_SIGNAL)
        {
            sFatalSignalHook();   // #876: async-signal-safe by contract
        }
        {
            char h[19];
            fatal_write("\n*** fatal signal ");
            fatal_hex((unsigned long long)signum, h);
            fatal_write(h);
            fatal_write(" si_code ");
            fatal_hex((unsigned long long)(long long)(info ? info->si_code : 0), h);
            fatal_write(h);
            fatal_write(" ***\n");
            fatal_write_exe_header();
        }
#if LL_LINUX && defined(__x86_64__)
        // Fatal-signal context dump: faulting pc/sp/address plus the raw stack
        // words -- a truncated core loses the stack, and a call through a null
        // pointer (pc=0) leaves the unwinder nothing; the caller's return address
        // is the word at sp. Values inside the main executable are also printed as
        // in-binary offsets ("exe+0x...") for addr2line against the archived debug
        // file named in the header.
        if ((signum == SIGSEGV || signum == SIGBUS || signum == SIGILL || signum == SIGFPE) && uctx)
        {
            const ucontext_t* uc = (const ucontext_t*)uctx;
            const unsigned long long pc = (unsigned long long)uc->uc_mcontext.gregs[REG_RIP];
            const unsigned long long sp = (unsigned long long)uc->uc_mcontext.gregs[REG_RSP];
            const unsigned long long bp = (unsigned long long)uc->uc_mcontext.gregs[REG_RBP];
            char h[19];
            fatal_write("*** fatal signal context: pc sp bp addr ***\n");
            fatal_write("pc ");   fatal_hex(pc, h); fatal_write(h); fatal_write_exe_offset(pc);
            fatal_write("\nsp ");  fatal_hex(sp, h); fatal_write(h);
            fatal_write("\nbp ");  fatal_hex(bp, h); fatal_write(h);
            fatal_write("\naddr "); fatal_hex((unsigned long long)(info ? info->si_addr : nullptr), h); fatal_write(h);
            fatal_write("\n*** stack words from sp (return addresses among them) ***\n");
            const unsigned long long* w = (const unsigned long long*)sp;
            for (int i = 0; sp && i < 48; ++i)
            {
                fatal_hex(w[i], h);
                fatal_write(h);
                fatal_write_exe_offset(w[i]);
                fatal_write("\n");
            }
        }
#endif
#if LL_LINUX
        {
            // Module+offset per frame. For the main executable the "(+0x...)" /
            // "(symbol+0x...)" offsets are in-binary already (PIE); the bracketed
            // value is the runtime address (subtract the load base above).
            fatal_write("*** backtrace ***\n");
            void* frames[128];
            const int n = backtrace(frames, 128);
            backtrace_symbols_fd(frames, n, STDERR_FILENO);
            fatal_write("*** end fatal signal context ***\n");
        }
#else
        (void)uctx;
#endif
        // Deliberately NOT LLApp::setError(): see the invariants block. The
        // process is about to dump core; nothing needs the status broadcast.
        //
        // begin_fatal_signal() already reset the disposition to SIG_DFL. raise()
        // only pends the (blocked) signal; it is delivered on return, at sigreturn,
        // so the core's frame #0 is the faulting instruction rather than raise().
        // The watchdog stays armed in case that somehow does not happen.
        if (signum == LL_SMACKDOWN_SIGNAL)
        {
            fatal_die_now(SIGABRT); // smackdown's default action has no core
        }
        raise(signum);
        return;
    }

    // Non-fatal signals. Logging here is only enabled in non-release builds.
    if (LLApp::sLogInSignal)
    {
        LL_INFOS() << "Signal handler - Got signal " << signum << " - " << apr_signal_description_get(signum) << LL_ENDL;
    }

    switch (signum)
    {
    case SIGCHLD:
    case SIGHUP:
        if (LLApp::sLogInSignal)
        {
            LL_INFOS() << "Signal handler - Got SIGCHLD or SIGHUP from " << info->si_pid << LL_ENDL;
        }
        return;
    case SIGINT:
    case SIGTERM:
        if (LLApp::sLogInSignal)
        {
            LL_WARNS() << "Signal handler - Got SIGINT, or TERM, exiting gracefully" << LL_ENDL;
        }
        // Graceful exit
        // Just set our state to quitting, not error
        if (LLApp::isQuitting() || LLApp::isError())
        {
            // We're already trying to die, just ignore this signal
            if (LLApp::sLogInSignal)
            {
                LL_INFOS() << "Signal handler - Already trying to quit, ignoring signal!" << LL_ENDL;
            }
            return;
        }
        LLApp::setQuitting();
        return;
    case SIGALRM:
    case SIGPIPE:
    case SIGUSR2:
    default:
        if (LLApp::sLogInSignal)
        {
            LL_INFOS() << "Signal handler - Unhandled signal " << signum << ", ignoring!" << LL_ENDL;
        }
        return;
    }
}

bool unix_post_minidump_callback(const char *dump_dir,
                      const char *minidump_id,
                      void *context, bool succeeded)
{
    // Copy minidump file path into fixed buffer in the app instance to avoid
    // heap allocations in a crash handler.

    // path format: <dump_dir>/<minidump_id>.dmp
    auto dirPathLength = strlen(dump_dir);
    auto idLength = strlen(minidump_id);

    // The path must not be truncated.
    llassert((dirPathLength + idLength + 5) <= LLApp::MAX_MINDUMP_PATH_LENGTH);

    char * path = LLApp::instance()->getMiniDumpFilename();
    auto remaining = LLApp::MAX_MINDUMP_PATH_LENGTH;
    strncpy(path, dump_dir, remaining);
    remaining -= dirPathLength;
    path += dirPathLength;
    if (remaining > 0 && dirPathLength > 0 && path[-1] != '/')
    {
        *path++ = '/';
        --remaining;
    }
    if (remaining > 0)
    {
        strncpy(path, minidump_id, remaining);
        remaining -= idLength;
        path += idLength;
        strncpy(path, ".dmp", remaining);
    }

    LL_INFOS("CRASHREPORT") << "generated minidump: " << LLApp::instance()->getMiniDumpFilename() << LL_ENDL;
    LLApp::runErrorHandler();

#ifndef LL_RELEASE_FOR_DOWNLOAD
    clear_signals();
    return false;
#else
    return true;
#endif
}
#endif // !WINDOWS

