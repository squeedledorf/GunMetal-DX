/**
 * @file llwindebug.cpp
 * @brief Windows debugging functions
 *
 * $LicenseInfo:firstyear=2004&license=viewerlgpl$
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
#include "llviewerprecompiledheaders.h"

#include "llwindebug.h"
#include "lldir.h"

#include <csignal>
#include <cstdlib>
#include <exception>
#include <typeinfo>

#include "llthread.h"
#include "llvkhitch.h"     // #757: the main thread's open frame sections (crash breadcrumb)
#include "vkgfx/vk_pipeline.h"   // #782: pipeline compiles in flight (crash breadcrumb)

#include <cstring>
#include <psapi.h>


// based on dbghelp.h
typedef bool (WINAPI *MINIDUMPWRITEDUMP)(HANDLE hProcess, DWORD dwPid, HANDLE hFile, MINIDUMP_TYPE DumpType,
                                    CONST PMINIDUMP_EXCEPTION_INFORMATION ExceptionParam,
                                    CONST PMINIDUMP_USER_STREAM_INFORMATION UserStreamParam,
                                    CONST PMINIDUMP_CALLBACK_INFORMATION CallbackParam
                                    );

MINIDUMPWRITEDUMP f_mdwp = NULL;


class LLMemoryReserve {
public:
    LLMemoryReserve();
    ~LLMemoryReserve();
    void reserve();
    void release();
private:
    unsigned char *mReserve;
    static const size_t MEMORY_RESERVATION_SIZE;
};

LLMemoryReserve::LLMemoryReserve() :
    mReserve(NULL)
{
}

LLMemoryReserve::~LLMemoryReserve()
{
    release();
}

// I dunno - this just seemed like a pretty good value.
const size_t LLMemoryReserve::MEMORY_RESERVATION_SIZE = 5 * 1024 * 1024;

void LLMemoryReserve::reserve()
{
    if(NULL == mReserve)
    {
        mReserve = new unsigned char[MEMORY_RESERVATION_SIZE];
    }
}

void LLMemoryReserve::release()
{
    if (NULL != mReserve)
    {
        delete [] mReserve;
    }
    mReserve = NULL;
}

static LLMemoryReserve gEmergencyMemoryReserve;


// ---------------------------------------------------------------------------------
// Crash report in the log (#690 / #694). A minidump stays on the user's machine; the
// bug report only carries the log. So before the dump, one "#Crash#" WARNING says
// what failed (exception code, faulting module + offset, thread) followed by the
// faulting thread's stack as module+offset frames, which symbolize against the
// release's PDB. Only plain buffers here: the process is already in trouble.

static volatile LONG sCrashReported = 0;    // one report per process (terminate -> abort)

static const char* exception_name(DWORD code)
{
    switch (code)
    {
        case EXCEPTION_ACCESS_VIOLATION:        return "access violation";
        case EXCEPTION_STACK_OVERFLOW:          return "stack overflow";
        case EXCEPTION_ILLEGAL_INSTRUCTION:     return "illegal instruction";
        case EXCEPTION_PRIV_INSTRUCTION:        return "privileged instruction";
        case EXCEPTION_IN_PAGE_ERROR:           return "in-page error";
        case EXCEPTION_INT_DIVIDE_BY_ZERO:      return "integer divide by zero";
        case EXCEPTION_INT_OVERFLOW:            return "integer overflow";
        case EXCEPTION_ARRAY_BOUNDS_EXCEEDED:   return "array bounds exceeded";
        case EXCEPTION_DATATYPE_MISALIGNMENT:   return "datatype misalignment";
        case EXCEPTION_NONCONTINUABLE_EXCEPTION: return "noncontinuable exception";
        case EXCEPTION_FLT_DIVIDE_BY_ZERO:
        case EXCEPTION_FLT_INVALID_OPERATION:
        case EXCEPTION_FLT_OVERFLOW:
        case EXCEPTION_FLT_UNDERFLOW:
        case EXCEPTION_FLT_STACK_CHECK:         return "floating point exception";
        case 0xC0000374:                        return "heap corruption";
        case 0xC0000409:                        return "stack buffer overrun / fail-fast";
        case 0xE06D7363:                        return "uncaught C++ exception";
        default:                                return "exception";
    }
}

// "module.dll+0x1234" (or the bare address outside any module) into out.
static void describe_address(DWORD64 addr, char* out, size_t out_size)
{
    HMODULE module = nullptr;
    if (addr && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(addr), &module) && module)
    {
        wchar_t path[MAX_PATH];
        const DWORD len = GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* base = path;
        for (DWORD i = 0; i < len && path[i]; ++i)
        {
            if (path[i] == L'\\' || path[i] == L'/')
            {
                base = path + i + 1;
            }
        }
        _snprintf_s(out, out_size, _TRUNCATE, "%ls+0x%llx", len ? base : L"?",
                    (unsigned long long)(addr - reinterpret_cast<DWORD64>(module)));
    }
    else
    {
        _snprintf_s(out, out_size, _TRUNCATE, "0x%llx", (unsigned long long)addr);
    }
}

// Appends up to max_frames "  #n module+0x..." lines for the stack that ctx describes.
// x64 unwind tables only (RtlVirtualUnwind); a frame without unwind data is a leaf
// whose return address is at [rsp]. Plain locals only: __try needs no destructors.
static size_t append_stack(CONTEXT ctx, size_t skip, size_t max_frames, char* out, size_t out_size, size_t used)
{
#if defined(_M_X64)
    size_t shown = 0;
    for (size_t depth = 0; depth < skip + max_frames && ctx.Rip && used + 1 < out_size; ++depth)
    {
        if (depth >= skip)
        {
            char where[MAX_PATH + 32];
            describe_address(ctx.Rip, where, sizeof(where));
            const int n = _snprintf_s(out + used, out_size - used, _TRUNCATE, "\n  #%u %s", (unsigned)shown, where);
            if (n < 0)
            {
                break;
            }
            used += (size_t)n;
            ++shown;
        }
        __try
        {
            DWORD64 image_base = 0;
            PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
            if (function)
            {
                PVOID handler_data = nullptr;
                DWORD64 establisher_frame = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, function, &ctx,
                                 &handler_data, &establisher_frame, nullptr);
            }
            else
            {
                ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
            }
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            break;  // unreadable stack: what was walked so far stands
        }
    }
#endif
    return used;
}

// #736: the faulting thread's OS name (SetThreadDescription: our workers name themselves,
// ll_set_current_thread_name; drivers often name theirs). Resolved at init, not in the
// crash path; Windows 10 1607+ (older: no name).
typedef HRESULT (WINAPI *GETTHREADDESCRIPTION)(HANDLE, PWSTR*);
static GETTHREADDESCRIPTION f_gtd = NULL;

// "'name'" or "(unnamed)" into out. GetThreadDescription allocates (LocalAlloc): the
// one heap use in the report, after the emergency reserve was released.
static void describe_thread(DWORD thread_id, char* out, size_t out_size)
{
    _snprintf_s(out, out_size, _TRUNCATE, "(unnamed)");
    if (!f_gtd)
    {
        return;
    }
    HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, thread_id);
    if (!thread)
    {
        return;
    }
    PWSTR desc = nullptr;
    if (SUCCEEDED(f_gtd(thread, &desc)) && desc)
    {
        if (desc[0])
        {
            _snprintf_s(out, out_size, _TRUNCATE, "'%ls'", desc);
        }
        LocalFree(desc);
    }
    CloseHandle(thread);
}

// #757 / #754 / #736: what the main thread was doing (its open frame sections, innermost
// last: LLVKHitch::breadcrumb), the frame, the teleport phase and the memory state, as
// "\n  context: ..." lines. A worker / driver fault's stack says nothing about the main
// thread; a main-thread stack without the release PDB says little; and the excerpt filter
// can drop the lines before the fault. Plain buffers and syscalls only.
static size_t append_breadcrumb(char* out, size_t out_size, size_t used)
{
    if (used + 1 >= out_size)
    {
        return used;
    }
    const LLVKHitch::Breadcrumb& bc = LLVKHitch::breadcrumb();
    int n = _snprintf_s(out + used, out_size - used, _TRUNCATE,
                        "\n  context: frame %u, teleport state %d for %u frames, VRAM %u / %u MB (renderer budget)",
                        bc.frame, bc.teleport, bc.tp_frames, bc.vram_used_mb, bc.vram_budget_mb);
    used += n > 0 ? (size_t)n : 0;
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    PROCESS_MEMORY_COUNTERS_EX pmc;
    std::memset(&pmc, 0, sizeof(pmc));
    pmc.cb = sizeof(pmc);
    if (used + 1 < out_size && GlobalMemoryStatusEx(&ms)
        && GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pmc), sizeof(pmc)))
    {
        n = _snprintf_s(out + used, out_size - used, _TRUNCATE,
                        ", RAM private %llu MB, physical avail %llu / %llu MB, commit avail %llu MB",
                        (unsigned long long)(pmc.PrivateUsage >> 20), (unsigned long long)(ms.ullAvailPhys >> 20),
                        (unsigned long long)(ms.ullTotalPhys >> 20), (unsigned long long)(ms.ullAvailPageFile >> 20));
        used += n > 0 ? (size_t)n : 0;
    }
    if (used + 1 >= out_size)
    {
        return used;
    }
    const int depth = bc.depth.load(std::memory_order_relaxed);
    n = _snprintf_s(out + used, out_size - used, _TRUNCATE, "\n  main thread in (%d open):", depth);
    used += n > 0 ? (size_t)n : 0;
    const int shown = depth < 0 ? 0 : (depth > LLVKHitch::Breadcrumb::kDepth ? LLVKHitch::Breadcrumb::kDepth : depth);
    for (int i = 0; i < shown && used + 1 < out_size; ++i)
    {
        const char* name = bc.open[i];
        n = _snprintf_s(out + used, out_size - used, _TRUNCATE, "%s%s", i ? " > " : " ", name ? name : "?");
        used += n > 0 ? (size_t)n : 0;
    }
    if (shown == 0 && used + 1 < out_size)
    {
        n = _snprintf_s(out + used, out_size - used, _TRUNCATE, " (between timed sections)");
        used += n > 0 ? (size_t)n : 0;
    }
    // #782: the pipelines being compiled right now (a crash inside the driver's shader
    // compiler runs on a driver thread and names none). Static buffers, no lock.
    if (used + 1 < out_size)
    {
        static char compiles[2048];
        vkPipelineCompilesInFlightText(compiles, sizeof(compiles));
        n = _snprintf_s(out + used, out_size - used, _TRUNCATE, "\n  pipeline compiles in flight: %s", compiles);
        used += n > 0 ? (size_t)n : 0;
    }
    // #886 / #887: the NVIDIA driver also compiles on its own threads after vkCreate*Pipelines
    // returned (an int3 in nvgpucomp64 on a driver thread, nothing in flight): the pipelines
    // created last are the candidates.
    if (used + 1 < out_size)
    {
        static char recent[3072];
        vkPipelineRecentCompilesText(recent, sizeof(recent));
        n = _snprintf_s(out + used, out_size - used, _TRUNCATE, "\n  pipelines created last (newest first): %s", recent);
        used += n > 0 ? (size_t)n : 0;
    }
    return used;
}

// reason: what kind of failure; ep: the exception, or null to walk the caller's stack.
static void log_crash_report(const char* reason, PEXCEPTION_POINTERS ep, DWORD thread_id, bool main_thread)
{
    static char report[8192];   // static: this can run on an overflowed stack's last pages
    size_t used = 0;
    const char* thread_kind = main_thread ? "main" : "worker";
    char thread_name[160];
    describe_thread(thread_id, thread_name, sizeof(thread_name));
    if (ep && ep->ExceptionRecord)
    {
        const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
        char where[MAX_PATH + 32];
        describe_address(reinterpret_cast<DWORD64>(rec->ExceptionAddress), where, sizeof(where));
        int n = _snprintf_s(report, sizeof(report), _TRUNCATE, "Crash: %s 0x%08lx (%s) at %s, %s thread %lu %s",
                            reason, (unsigned long)rec->ExceptionCode, exception_name(rec->ExceptionCode),
                            where, thread_kind, (unsigned long)thread_id, thread_name);
        used = n > 0 ? (size_t)n : 0;
        if ((rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || rec->ExceptionCode == EXCEPTION_IN_PAGE_ERROR)
            && rec->NumberParameters >= 2 && used + 1 < sizeof(report))
        {
            const ULONG_PTR op = rec->ExceptionInformation[0];
            n = _snprintf_s(report + used, sizeof(report) - used, _TRUNCATE, "; %s address 0x%llx",
                            op == 0 ? "reading" : (op == 1 ? "writing" : "executing"),
                            (unsigned long long)rec->ExceptionInformation[1]);
            used += n > 0 ? (size_t)n : 0;
        }
        if (ep->ContextRecord)
        {
            used = append_stack(*ep->ContextRecord, 0, 40, report, sizeof(report), used);
        }
    }
    else
    {
        int n = _snprintf_s(report, sizeof(report), _TRUNCATE, "Crash: %s, %s thread %lu %s",
                            reason, thread_kind, (unsigned long)thread_id, thread_name);
        used = n > 0 ? (size_t)n : 0;
        CONTEXT ctx;
        RtlCaptureContext(&ctx);
        used = append_stack(ctx, 0, 40, report, sizeof(report), used);
    }
    // #876: a crash on a pipeline compile thread (inside the driver's shader compiler): name the
    // pipeline that thread was compiling, so a driver bug points at one shader.
    if (used + 1 < sizeof(report))
    {
        static char compiling[128];
        if (vkPipelineCompileOnThread((uint64_t)thread_id, compiling, sizeof(compiling)))
        {
            const int n = _snprintf_s(report + used, sizeof(report) - used, _TRUNCATE,
                                      "\n  crashed while compiling pipeline: %s", compiling);
            used += n > 0 ? (size_t)n : 0;
        }
    }
    used = append_breadcrumb(report, sizeof(report), used);
    LL_WARNS("Crash") << report << LL_ENDL;
}

// #876 / #890: does this crash say "the driver's shader compiler fell over"? True when the
// faulting thread holds a pipeline-compile crumb (a "VK pipeline compile" worker), or the
// faulting address is inside a driver compiler module (a crash on a driver-owned thread:
// the NVIDIA int3 in nvgpucomp64, #890). Plain buffers only; no allocation.
static bool crash_in_pipeline_compile(PEXCEPTION_POINTERS ep, DWORD thread_id)
{
    if (vkPipelineCrashOnCompileThread((uint64_t)thread_id))
    {
        return true;
    }
    if (!ep || !ep->ExceptionRecord || !ep->ExceptionRecord->ExceptionAddress)
    {
        return false;
    }
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(ep->ExceptionRecord->ExceptionAddress), &module) || !module)
    {
        return false;
    }
    wchar_t path[MAX_PATH];
    const DWORD len = GetModuleFileNameW(module, path, MAX_PATH);
    const wchar_t* base = path;
    for (DWORD i = 0; i < len && path[i]; ++i)
    {
        if (path[i] == L'\\' || path[i] == L'/')
        {
            base = path + i + 1;
        }
    }
    static const wchar_t* const kCompilerModules[] = {
        L"igvk64.dll", L"igvk32.dll", L"nvgpucomp64.dll", L"nvgpucomp32.dll", L"amdvlk64.dll", L"amdxc64.dll",
    };
    for (const wchar_t* name : kCompilerModules)
    {
        if (_wcsicmp(base, name) == 0)
        {
            return true;
        }
    }
    return false;
}

// Log + minidump, once per process.
static void report_crash(const char* reason, PEXCEPTION_POINTERS ep, DWORD thread_id, bool main_thread)
{
    if (InterlockedExchange(&sCrashReported, 1) != 0)
    {
        return;
    }
    gEmergencyMemoryReserve.release();  // room for the log line if the heap ran dry
    // #876 / #890: a crash during pipeline compilation arms "compile one at a time" for the
    // next start (before the log + dump: the marker is a few bytes and must not be lost to them).
    // #1064: it also names the driver and the pipeline the crashing thread was compiling, so a
    // second crash on the same pipeline + driver quarantines that pipeline.
    const bool compile_crash = crash_in_pipeline_compile(ep, thread_id);
    const bool marker_written = compile_crash && vkPipelineCrashMarkerWrite((uint64_t)thread_id);
    log_crash_report(reason, ep, thread_id, main_thread);
    if (compile_crash)
    {
        LL_WARNS("Crash") << "Crash during pipeline compilation: "
                          << (marker_written ? "marker written, the next start compiles one pipeline at a time (#876/#890)"
                                             : "could not write the marker (#876/#890)") << LL_ENDL;
    }
    LLWinDebug::generateMinidump(ep, thread_id);
}

static void report_crash_here(const char* reason)
{
    report_crash(reason, nullptr, GetCurrentThreadId(), on_main_thread());
}

struct CrashReportJob
{
    PEXCEPTION_POINTERS mExceptionInfo;
    DWORD               mThreadId;
    bool                mMainThread;
};

static DWORD WINAPI crash_report_thread(LPVOID param)
{
    const CrashReportJob* job = static_cast<const CrashReportJob*>(param);
    report_crash("unhandled exception", job->mExceptionInfo, job->mThreadId, job->mMainThread);
    return 0;
}

// Unhandled-exception filter (NOT a vectored handler: those see every first-chance
// exception, including C++ exceptions that are caught normally). Logs the crash,
// writes logs/Slipstream.dmp, then lets the default handling terminate the process.
LONG WINAPI unhandledFilter(PEXCEPTION_POINTERS exception_infop)
{
    const DWORD thread_id = GetCurrentThreadId();
    const bool main_thread = on_main_thread();
    if (exception_infop && exception_infop->ExceptionRecord
        && exception_infop->ExceptionRecord->ExceptionCode == EXCEPTION_STACK_OVERFLOW)
    {
        // An overflowed stack has only the guaranteed pages left: report from a fresh
        // thread (the dump names the faulting thread) and wait for it.
        CrashReportJob job = { exception_infop, thread_id, main_thread };
        HANDLE thread = CreateThread(nullptr, 1024 * 1024, &crash_report_thread, &job, 0, nullptr);
        if (thread)
        {
            WaitForSingleObject(thread, 60 * 1000);
            CloseHandle(thread);
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    report_crash("unhandled exception", exception_infop, thread_id, main_thread);
    return EXCEPTION_CONTINUE_SEARCH;
}

// Uncaught C++ exceptions end in std::terminate, not an SEH crash.
static void terminateHandler()
{
    char reason[512] = "std::terminate";
    if (std::exception_ptr current = std::current_exception())
    {
        try
        {
            std::rethrow_exception(current);
        }
        catch (const std::exception& e)
        {
            _snprintf_s(reason, sizeof(reason), _TRUNCATE, "std::terminate, uncaught %s: %s", typeid(e).name(), e.what());
        }
        catch (...)
        {
            _snprintf_s(reason, sizeof(reason), _TRUNCATE, "std::terminate, uncaught non-std exception");
        }
    }
    report_crash_here(reason);
    std::abort();
}

// The release CRT fail-fasts on these, which skips the unhandled-exception filter:
// without a handler such a crash leaves no line in the log (#690). Report, then end
// the process as the CRT would have.
static void __cdecl invalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t)
{
    report_crash_here("CRT invalid parameter");
    TerminateProcess(GetCurrentProcess(), 0xC0000417);   // STATUS_INVALID_CRUNTIME_PARAMETER
}

static void __cdecl purecallHandler()
{
    report_crash_here("pure virtual function call");
    TerminateProcess(GetCurrentProcess(), 3);
}

// abort() (an assert, a library giving up): raises SIGABRT before it fail-fasts.
// Returning lets abort() finish as before (Windows Error Reporting still sees it).
static void abortSignalHandler(int)
{
    report_crash_here("abort()");
}

// static
void  LLWinDebug::initSingleton()
{
    static bool s_first_run = true;
    // Load the dbghelp dll now, instead of waiting for the crash.
    // Less potential for stack mangling

    // Don't install vectored exception handler if being debugged.
    if(IsDebuggerPresent()) return;

    if (s_first_run)
    {
        // First, try loading from the directory that the app resides in.
        std::string local_dll_name = gDirUtilp->findFile("dbghelp.dll", gDirUtilp->getWorkingDir(), gDirUtilp->getExecutableDir());

        HMODULE hDll = NULL;
        hDll = LoadLibraryA(local_dll_name.c_str());
        if (!hDll)
        {
            hDll = LoadLibrary(L"dbghelp.dll");
        }

        if (!hDll)
        {
            LL_WARNS("AppInit") << "Couldn't find dbghelp.dll!" << LL_ENDL;
        }
        else
        {
            f_mdwp = (MINIDUMPWRITEDUMP) GetProcAddress(hDll, "MiniDumpWriteDump");

            if (!f_mdwp)
            {
                FreeLibrary(hDll);
                hDll = NULL;
            }
        }

        gEmergencyMemoryReserve.reserve();

        // #736: thread names in the crash line (not resolved in the crash path).
        if (HMODULE k32 = GetModuleHandleW(L"kernel32.dll"))
        {
            f_gtd = (GETTHREADDESCRIPTION) GetProcAddress(k32, "GetThreadDescription");
        }

        s_first_run = false;

        // Save a Windows minidump on a crash (no BugSplat in these builds).
        SetUnhandledExceptionFilter(&unhandledFilter);
        std::set_terminate(&terminateHandler);
        // The CRT's own fatal paths fail-fast past the filter: route them through the
        // same report (#690). Heap corruption (0xC0000374) and __fastfail still can't be
        // caught in-process; Windows Error Reporting (kept on) records those.
        _set_invalid_parameter_handler(&invalidParameterHandler);
        _set_purecall_handler(&purecallHandler);
        signal(SIGABRT, &abortSignalHandler);
        // A stack overflow on the main thread leaves the filter enough stack to start
        // the reporting thread.
        ULONG stack_guarantee = 64 * 1024;
        SetThreadStackGuarantee(&stack_guarantee);
    }
}

void LLWinDebug::cleanupSingleton()
{
    gEmergencyMemoryReserve.release();
}

void LLWinDebug::writeDumpToFile(MINIDUMP_TYPE type, MINIDUMP_EXCEPTION_INFORMATION *ExInfop, const std::string& filename)
{
    // Only reached from the unhandled-exception filter / terminate handler, so
    // regular (caught) C++ exceptions never write a dump.
    if ( true )
    {
        if(f_mdwp == NULL || gDirUtilp == NULL)
        {
            return;
        }
        else
        {
            std::string dump_path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS, filename);

            HANDLE hFile = CreateFileA(dump_path.c_str(),
                                        GENERIC_WRITE,
                                        FILE_SHARE_WRITE,
                                        NULL,
                                        CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL,
                                        NULL);

            if (hFile != INVALID_HANDLE_VALUE)
            {
                // Write the dump, ignoring the return value
                f_mdwp(GetCurrentProcess(),
                        GetCurrentProcessId(),
                        hFile,
                        type,
                        ExInfop,
                        NULL,
                        NULL);

                CloseHandle(hFile);
                LL_WARNS("Crash") << "Crash minidump written: " << dump_path << LL_ENDL;
            }

        }
    }
}

// static
void LLWinDebug::generateMinidump(struct _EXCEPTION_POINTERS *exception_infop, unsigned long thread_id)
{
    std::string dump_path = gDirUtilp->getExpandedFilename(LL_PATH_LOGS,
                                                "SlipstreamException");
    gEmergencyMemoryReserve.release();
    const MINIDUMP_TYPE type = (MINIDUMP_TYPE)(MiniDumpWithDataSegs | MiniDumpWithIndirectlyReferencedMemory | MiniDumpWithThreadInfo);
    if (exception_infop)
    {
        _MINIDUMP_EXCEPTION_INFORMATION ExInfo;
        ExInfo.ThreadId = thread_id ? (DWORD)thread_id : ::GetCurrentThreadId();   // the faulting thread
        ExInfo.ExceptionPointers = exception_infop;
        ExInfo.ClientPointers = NULL;
        writeDumpToFile(type, &ExInfo, "Slipstream.dmp");
    }
    else
    {
        writeDumpToFile(type, nullptr, "Slipstream.dmp");   // terminate: no exception record
    }
}
