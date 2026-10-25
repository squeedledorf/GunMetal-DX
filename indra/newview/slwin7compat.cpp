/**
 * @file slwin7compat.cpp
 * @brief Windows 7: keep the executable free of Windows 8+ kernel32 imports.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
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
 * $/LicenseInfo$
 */

// The prebuilt minizip-ng (colladadom's zip reader) was compiled for Windows 8+
// and calls CreateFile2 through the import pointer __imp_CreateFile2. A loader
// that can't resolve an import refuses to start the process, so on Windows 7 the
// viewer died with "The procedure entry point CreateFile2 could not be located".
// Defining that import pointer here makes the linker use ours instead of
// kernel32's: the executable no longer imports CreateFile2 at all. The shim calls
// the real CreateFile2 when the system has it, else the equivalent CreateFileW.

#include <windows.h>

namespace
{
    // CREATEFILE2_EXTENDED_PARAMETERS is only declared for _WIN32_WINNT >= 0x0602.
    struct SLCreateFile2Params
    {
        DWORD dwSize;
        DWORD dwFileAttributes;
        DWORD dwFileFlags;
        DWORD dwSecurityQosFlags;
        LPSECURITY_ATTRIBUTES lpSecurityAttributes;
        HANDLE hTemplateFile;
    };

    using CreateFile2Fn = HANDLE (WINAPI*)(LPCWSTR, DWORD, DWORD, DWORD, void*);

    HANDLE WINAPI sl_CreateFile2(LPCWSTR name, DWORD access, DWORD share, DWORD disposition, void* params)
    {
        static const CreateFile2Fn real = reinterpret_cast<CreateFile2Fn>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "CreateFile2"));
        if (real)
        {
            return real(name, access, share, disposition, params);
        }
        const SLCreateFile2Params* p = static_cast<const SLCreateFile2Params*>(params);
        const DWORD flags = p ? (p->dwFileAttributes | p->dwFileFlags | p->dwSecurityQosFlags) : 0;
        return CreateFileW(name, access, share, p ? p->lpSecurityAttributes : nullptr, disposition, flags,
                           p ? p->hTemplateFile : nullptr);
    }
}

extern "C"
{
    // The import pointer the minizip objects reference (x64: no leading underscore).
    void* __imp_CreateFile2 = reinterpret_cast<void*>(&sl_CreateFile2);
}
